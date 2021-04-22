#include "ls_proxy.h"


Server::Server(Connection* _conn):
    conn{_conn}, queued_output_f{new Circularbuf()}, response_buf_s{NULL}
{
    int sock = connect_TCP(Server::address, Server::port);
    if (sock < 0) {
        WARNING("Server down or having network issue.");
        throw ConnectionError();
    }
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    LOG2("Connected to [SERVER] (active: %d)\n", ++Server::connection_count);
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
    if (queued_output_f) delete queued_output_f;
    if (response_buf_s) delete response_buf_s;
}

void Server::recv_all_to_buffer(int fd) {

    // int count = read(fd, read_buffer, sizeof(read_buffer));
    // server->filebuf->store(read_buffer, count);
    // LOG2("%11d bytes << [SERVER]\n", count);

    // if (count == 0 || server->check_response_completed(count)) {
    //     // start replying to client
    //     del_event(server->read_evt);
    //     server->filebuf->rewind();
    //     delete server;  // destroy server
    //     client->server = NULL;
    //     add_event(client->write_evt);
    // }
}

// bool Server::check_response_completed(int last_read_count) {
    // TODO: HEAD method can has content-length, but no body
    // TODO: 304 Not Modified can has transfer-encoding, but no body

    // TODO: detect transfer encoding first
// }

void Server::on_readable(int fd, short/*flag*/, void* arg) {
    auto server = (Server*)arg;
    auto conn = server->conn;
    if (conn->is_fast_mode()) {
        assert(conn->client);  // client always exists whenever server has msg
        conn->fast_forward(server, conn->client);
    } else {  // slow mode
        server->recv_all_to_buffer(fd);
    }
}

void Server::on_writable(int/*fd*/, short/*flag*/, void* arg) {
    auto server = (Server*)arg;
    auto conn = server->conn;
    if (conn->is_fast_mode()) {
        // add back client's read_evt because we removed it before
        del_event(server->write_evt);
        add_event(conn->client->read_evt);
        LOG2("[%s] Server writable again.\n", conn->client->addr.c_str());
    } else {
        // TODO
    }
}
