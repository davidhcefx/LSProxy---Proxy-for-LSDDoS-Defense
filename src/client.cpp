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
    close(get_fd());
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    free_hybridbuf.push(request_history);
    if (queued_output_f) delete queued_output_f;
    if (request_buf_s) delete request_buf_s;
}

void Client::keep_track_request_history(const char* data, size_t size) {
    if (!conn->do_parse(data, size)) return;  // parser error
    auto start_ptr = conn->get_cur_request_start();
    if (data <= start_ptr && start_ptr < data + size) {
        // start a new recording
        request_history->clear();
        request_history->store(start_ptr, size - (start_ptr - data));
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
            conn->init_parser();
            conn->server = new Server(conn);
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
            LOG2("[%s] Added back server->read_evt\n", client->addr.c_str());
        }
    } else {
        // TODO
    }

//     int count = client->filebuf->fetch(read_buffer, sizeof(read_buffer));
//     if (count <= 0) {
//         // done replying
//         if (client->server) {
//             delete client->server;
//         }
//         delete client;
//         return;
//     }
//     write(fd, read_buffer, count);
// #ifdef LOG_LEVEL_2
//     char buf[51];
//     int n = min(count, 50);
//     memcpy(buf, read_buffer, n);
//     buf[n] = '\0';
//     LOG2("[%s] << '%s'...\n", client->addr.c_str(), replace_newlines(buf));
// #endif
}
