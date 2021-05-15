#include "ls_proxy.h"


Client::Client(int fd, const struct sockaddr_in& _addr, Connection* _conn):
    addr{get_host_and_port(_addr)}, conn{_conn}, queued_output{NULL},
    recv_count{0}, request_buf{NULL}, request_tmp_buf{NULL}, response_buf{NULL}
{
    read_evt = new_read_event(fd, Client::on_readable, this);
    write_evt = new_write_event(fd, Client::on_writable, this);
    request_history = free_hybridbuf.front();
    free_hybridbuf.pop();
    LOG2("[%s] Allocated hist buf (#%d)\n", c_addr(), request_history->get_fd());
}

Client::~Client() {
    LOG1("[%s] Connection closed.\n", c_addr());
    del_event(read_evt);  // should be called before closing fd
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    close_socket_gracefully(get_fd());
    free_hybridbuf.push(request_history);
    if (queued_output) delete queued_output;
    if (request_buf) delete request_buf;
    if (request_tmp_buf) delete request_tmp_buf;
    if (response_buf) delete response_buf;
}

void Client::recv_to_buffer_slowly(int fd) {
    auto stat = read_all(fd, global_buffer, sizeof(global_buffer));
    LOG2("[%s] %6lu >>>> request |\n", c_addr(), stat.nbytes);
    if (stat.has_eof) {  // client closed
        delete conn;
        return;
    }
    try {
        conn->parser->do_parse(global_buffer, stat.nbytes);
    } catch (ParserError& err) {
        // close connection
        assert(!conn->server);  // server only exists when client paused
        conn->free_parser();
        if (response_buf->data_size > 0) {
            set_reply_only_mode();
        } else {
            delete conn;
        }
        return;
    }
    if (auto end_ptr = conn->parser->get_first_end_of_msg()) {
        LOG2("[%s] Request completed.\n", c_addr());
        // store first msg to request_buf, the remaining to request_tmp_buf
        auto part1_size = end_ptr - global_buffer;
        request_buf->store(global_buffer, part1_size);
        if (stat.nbytes - part1_size > 0) { [[unlikely]]
            request_tmp_buf->store(end_ptr, stat.nbytes - part1_size);
        }
        // pause client, rewind buffers and interact with server
        pause_rw();
        request_buf->rewind();  // for further reads
        conn->parser->switch_to_response_mode();
        try {
            conn->server = new Server(conn);
        } catch (ConnectionError& err) {
            reply_with_503_unavailable(fd);
            delete conn;
            return;
        }
        add_event(conn->server->write_evt);
        add_event(conn->server->read_evt);
    } else {
        request_buf->store(global_buffer, stat.nbytes);
    }
}

void Client::send_response_slowly(int fd) {
    auto count = response_buf->fetch(global_buffer, sizeof(global_buffer));
    if (count > 0) {
        auto stat = write_all(fd, global_buffer, count);
        if (stat.nbytes < (size_t)count) [[unlikely]] {
            response_buf->rewind(count - stat.nbytes);
        }
        LOG2("[%s] %6lu <<<< response |\n", c_addr(), stat.nbytes);
    } else if (count == 0) {  // no data to forward
        LOG2("[%s] %6s <<<< response |\n", c_addr(), "0");
        del_event(write_evt);
        if (!conn->parser) {  // parser freed (reply-only mode)
            delete conn;
        }
    }
}

void Client::keep_track_request_history(const char* data, size_t size) {
    // ignore previous complete-requests (if any)
    conn->parser->do_parse(data, size);
    if (auto end_ptr = conn->parser->get_last_end_of_msg()) {
        // start storing from here
        request_history->clear();
        request_history->store(end_ptr, size - (end_ptr - data));
        LOG2("History: Starts new record from '%s'\n",
             end_ptr < data + size ? end_ptr : "EOL");
    } else {
        request_history->store(data, size);
    }
}

void Client::on_readable(int fd, short/*flag*/, void* arg) {
    auto client = reinterpret_cast<Client*>(arg);
    auto conn = client->conn;
    if (conn->is_fast_mode()) {
        if (!conn->parser) { [[unlikely]]  // parser & server not created
            conn->parser = new HttpParser();
            try {
                conn->server = new Server(conn);
            } catch (ConnectionError& err) {
                reply_with_503_unavailable(fd);
                delete conn;
                return;
            }
            conn->server->queued_output = new Circularbuf();
            client->queued_output = new Circularbuf();
            add_event(conn->server->read_evt);
        }
        conn->fast_forward(client, conn->server);
    } else {
        /* Slow mode */
        client->recv_to_buffer_slowly(fd);
    }
}

void Client::on_writable(int fd, short/*flag*/, void* arg) {
    auto client = reinterpret_cast<Client*>(arg);
    auto conn = client->conn;
    if (conn->is_fast_mode()) {
        if (!conn->server) { [[unlikely]]  // server closed (reply-only mode)
            auto stat = client->queued_output->write_all_to(fd);
            if ((stat.has_error && errno != EAGAIN && errno != EINTR) \
                    || (client->queued_output->data_size() == 0)) {
                delete conn;
            }
        } else {
            // add back server's recv because we removed it before
            add_event(conn->server->read_evt);
            del_event(client->write_evt);
            // write some
            auto stat = client->queued_output->write_all_to(fd);
            LOG2("[%s] %6lu <<<< queue(%lu)\n", client->c_addr(),
                 stat.nbytes, client->queued_output->data_size());
        }
    } else {
        /* Slow mode */
        client->send_response_slowly(fd);
    }
}
