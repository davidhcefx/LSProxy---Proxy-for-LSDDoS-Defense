#include "ls_proxy.h"


Server::Server(Connection* _conn): conn{_conn}, queued_output{NULL} {
    int sock = connect_TCP(Server::address, Server::port);
    if (sock < 0) { [[unlikely]]
        WARNING("Server down or having network issue.");
        throw ConnectionError();
    }
    if (evutil_make_socket_nonblocking(sock) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    LOG2("Connected to [SERVER] (#%d) (active: %d)\n", sock,
         ++Server::connection_count);
    read_evt = new_read_event(sock, Server::on_readable, this);
    write_evt = new_write_event(sock, Server::on_writable, this);
}

Server::~Server() {
    LOG2("Connection closed: [SERVER] (active: %u)\n", --Server::connection_count);
    close_socket_gracefully(get_fd());
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    if (queued_output) delete queued_output;
}

void Server::recv_to_buffer_slowly(int fd) {
    auto client = conn->client;
    auto stat = read_all(fd, global_buffer, sizeof(global_buffer));
    client->response_buf->store(global_buffer, stat.nbytes);
    LOG2("[%s] response <<<< %-6lu [SERVER]\n", client->addr.c_str(), stat.nbytes);
    try {
        conn->parser->do_parse(global_buffer, stat.nbytes);
    } catch (ParserError& err) {
        // close connection
        conn->free_server();
        conn->free_parser();
        if (client->response_buf->data_size > 0) {
            client->set_reply_only_mode();
            client->resume_rw();
        } else {
            delete conn;
        }
        return;
    }
    auto end_ptr = conn->parser->get_last_end_of_msg();
    if (stat.has_eof || end_ptr) {  // server closed or msg finished
        // resume client
        conn->free_server();
        conn->parser->switch_to_request_mode();
        client->resume_rw();
    }

    /* TODO: how about these?
     *  HEAD method can has content-length, but no body
     *  304 Not Modified can has transfer-encoding, but no body
     */
}

void Server::send_request_slowly(int fd) {
    auto client = conn->client;
    auto count = client->request_buf->fetch(global_buffer, sizeof(global_buffer));
    if (count > 0) {
        auto stat = write_all(fd, global_buffer, count);
        if (stat.nbytes < (size_t)count) {
            client->request_buf->rewind(count - stat.nbytes);
        }
        LOG2("[%s] request >>>> %-6lu [SERVER]\n", client->addr.c_str(), \
             count - stat.nbytes);
    } else if (count == 0) {  // done forwarding
        del_event(write_evt);
        // move data back from request_tmp_buf
        swap(client->request_buf, client->request_tmp_buf);
        client->request_tmp_buf->clear();
        /* request_buf's cursor already at the back */
    }
}

void Server::on_readable(int fd, short flag, void* arg) {
    auto server = (Server*)arg;
    auto conn = server->conn;
    auto client = conn->client;
    if (conn->is_fast_mode()) {
        assert(client);
        conn->fast_forward(server, client);
    } else if (conn->is_in_transition()) {
        if (flag & EV_TIMEOUT) {  // timed out (no packets for a while)
            conn->set_transition_done();
            conn->free_server();
            client->resume_rw();
        } else {
            // store to response buffer
            auto stat = read_all(fd, global_buffer, sizeof(global_buffer));
            if (stat.has_eof) {  // server closed
                conn->set_transition_done();
                conn->free_server();
                client->resume_rw();
                return;
            }
            client->response_buf->store(global_buffer, stat.nbytes);
            LOG2("[%s] response <<<< %-6lu [SERVER]\n", client->addr.c_str(), \
                 stat.nbytes);
        }
    } else {
        server->recv_to_buffer_slowly(fd);
    }
}

void Server::on_writable(int fd, short/*flag*/, void* arg) {
    auto server = (Server*)arg;
    auto conn = server->conn;
    auto client = conn->client;
    if (conn->is_fast_mode()) {
        // add back client's recv because we removed it before
        add_event(client->read_evt);
        del_event(server->write_evt);
        // write some
        auto stat = server->queued_output->write_all_to(fd);
        LOG2("[%s] queue(%lu) >>>> %-6lu [SERVER]\n", client->addr.c_str(),
             server->queued_output->data_size(), stat.nbytes);
    } else if (conn->is_in_transition()) {
        // simply feed queue to server
        auto stat = server->queued_output->write_all_to(fd);
        if (server->queued_output->data_size() == 0) {  // done
            del_event(server->write_evt);
            server->free_queued_output();
        }
        LOG2("[%s] queue(%lu) >>>> %-6lu [SERVER]\n", client->addr.c_str(),
             server->queued_output->data_size(), stat.nbytes);
    } else {
        server->send_request_slowly(fd);
    }
}
