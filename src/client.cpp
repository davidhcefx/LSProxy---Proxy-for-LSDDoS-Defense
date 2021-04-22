#include "ls_proxy.h"


Client::Client(int fd, const struct sockaddr_in& _addr, Connection* _conn):
    addr{get_host_and_port(_addr)}, conn{_conn}, queued_output_f{NULL},
    request_buf_s{new Filebuf()}
{
    read_evt = new_read_event(fd, Client::on_readable, this);
    write_evt = new_write_event(fd, Client::on_writable, this);
    request_history = free_hybridbuf.front();
    free_hybridbuf.pop();
}

Client::~Client() {
    LOG1("Connection closed: [%s]\n", addr.c_str());
    close_socket_gracefully(get_fd());
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    free_hybridbuf.push(request_history);
    if (queued_output_f) delete queued_output_f;
    if (request_buf_s) delete request_buf_s;
}

void Client::keep_track_request_history(const char* data, size_t size) {
    conn->do_parse(data, size);
    auto end_ptr = conn->get_last_request_end();
    if (data < end_ptr && end_ptr <= data + size) {
        // start a new recording
        request_history->clear();
        request_history->store(end_ptr, size - (end_ptr - data));
        LOG2("History: Starts new record from '%s'\n",
             end_ptr < data + size ? end_ptr : "EOL");
    } else {
        request_history->store(data, size);
    }
}

void Client::recv_to_buffer_slowly(int fd) {

//     int count = read(fd, read_buffer, sizeof(read_buffer));
//     if (count == 0) {
//         // premature close
//         if (client->server) {
//             delete client->server;
//         }
//         delete client;
//         return;
//     }
// #ifdef LOG_LEVEL_2
//     char buf[51];
//     int n = min(count, 50);
//     memcpy(buf, read_buffer, n);
//     buf[n] = '\0';
//     LOG2("[%s] >> '%s'...\n", client->addr.c_str(), replace_newlines(buf));
// #endif
//     client->filebuf->store(read_buffer, count);
//     if (client->check_request_completed(count)) {
//         // start forwarding to server
//         del_event(client->read_evt);  // disable further reads
//         client->filebuf->rewind();
//         Server* server = new Server(client);
//         add_event(server->write_evt);
//     }
}

void Client::on_readable(int fd, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    auto conn = client->conn;
    if (conn->is_fast_mode()) {
        if (conn->parser_uninitialized()) {
            try {
                conn->server = new Server(conn);
            } catch (ConnectionError& err) {
                reply_with_503_unavailable(fd);
                delete conn;
                return;
            }
            conn->init_parser();
            client->queued_output_f = new Circularbuf();
            add_event(conn->server->read_evt);
        }
        conn->fast_forward(client, conn->server);
    } else {  // slow mode
        client->recv_to_buffer_slowly(fd);
    }
}

void Client::on_writable(int/*fd*/, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    auto conn = client->conn;
    if (conn->is_fast_mode()) {
        if (!conn->server) {  // server closed (reply-only mode)
            client->queued_output_f->write_all_to(client->get_fd());
            if (client->queued_output_f->data_size() == 0) {
                delete conn;
            }
        } else {
            // add back server's read_evt because we removed it before
            del_event(client->write_evt);
            add_event(conn->server->read_evt);
            LOG2("[%s] Client writable again.\n", client->addr.c_str());
        }
    } else {
        // TODO
    }
}
