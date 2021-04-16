#include "ls_proxy.h"


Client::Client(int fd, const struct sockaddr_in& _addr, const Connection* _conn):
    addr{get_host_and_port(_addr)}, conn{_conn}, queued_output_f{NULL},
    request_buf_s{NULL}
{
    read_evt = new_read_event(fd, Client::recv_msg, this);
    write_evt = new_write_event(fd, Client::send_msg, this);
    request_hist = free_hybridbuf.front();
    free_hybridbuf.pop();
}

Client::~Client() {
    LOG1("Connection closed: [%s]\n", addr.c_str());
    close(get_fd());
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    free_hybridbuf.push(request_hist);
    if (queued_output_f) delete queued_output_f;
    if (request_buf_s) delete request_buf_s;
}

void Client::recv_to_buffer_slowly(int fd) {

    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count == 0) {
        // premature close
        if (client->server) {
            delete client->server;
        }
        delete client;
        return;
    }
#ifdef LOG_LEVEL_2
    char buf[51];
    int n = min(count, 50);
    memcpy(buf, read_buffer, n);
    buf[n] = '\0';
    LOG2("[%s] >> '%s'...\n", client->addr.c_str(), replace_newlines(buf));
#endif
    client->filebuf->store(read_buffer, count);
    if (client->check_request_completed(count)) {
        // start forwarding to server
        del_event(client->read_evt);  // disable further reads
        client->filebuf->rewind();
        Server* server = new Server(client);
        add_event(server->write_evt);
    }
}

bool Client::check_request_completed(int last_read_count) {
    if (header_len == 0) {  // header_len not set
        int offset = filebuf->search_double_crlf();
        if (offset == -1) {
            return false;
        } else if (offset == 0) {
            WARNING("Not ")
            return true;
        }
        header_len = offset;
    }
    if (content_len == 0) {  // content_len not set
        constexpr int digits = log10(MAX_BODY_SIZE) + 3;
        char res[digits];
        filebuf->search_header(CRLF "Content-Length:", res, header_len);
        content_len = atoi(res);  // TODO: atoi ?
        if (content_len == 0) {  // no Content-Length
            return true;
        }
    }
    // TODO: transfer encoding?

    if (content_len + header_len == data_size)

    if (content_len == 0) {
        return true;
    } else if (filebuf->data_size - )

    if (last_read_count == 5) return true;
    return false;

    // TODO: when it sees CRLF, it sets content_len if request type has body (beware of overflow)
}

void Client::recv_msg(int fd, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    auto conn = client->conn;
    if (conn->is_fast_mode()) {
        if (!conn->server) {  // uninitialized
            client->queued_output_f = new Circularbuf();
            conn->server = new Server(conn);
        }
        conn->fast_forward(client, conn->server);
    } else {  // slow mode
        client->recv_to_buffer_slowly(fd);
    }
}

void Client::send_msg(int fd, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    int count = client->filebuf->fetch(read_buffer, sizeof(read_buffer));
    if (count <= 0) {
        // done replying
        if (client->server) {
            delete client->server;
        }
        delete client;
        return;
    }
    write(fd, read_buffer, count);
#ifdef LOG_LEVEL_2
    char buf[51];
    int n = min(count, 50);
    memcpy(buf, read_buffer, n);
    buf[n] = '\0';
    LOG2("[%s] << '%s'...\n", client->addr.c_str(), replace_newlines(buf));
#endif
}
