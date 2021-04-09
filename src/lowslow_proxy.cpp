#include "lowslow_proxy.h"


queue<shared_ptr<Filebuf>> free_filebufs;
struct event_base* evt_base;
char read_buffer[10240];  // temporary buffer //TODO: what size is appropriate?
// class variables
char* Server::address;
unsigned short Server::port;
int Server::connection_count = 0;


Filebuf::Filebuf(): data_size{0}, next_pos{0} {
    buffer[0] = '\0';
    char name[] = "/tmp/lowslow_buf_XXXXXX";
    if ((fd = mkstemp(name)) < 0) {
        ERROR_EXIT("Cannot mkstemp");
    }
    if (make_file_nonblocking(fd) < 0) {
        ERROR_EXIT("Cannot make file nonblocking");
    }
    file_name = string(name);
    LOG2("#%d = '%s'\n", fd, name);
}

Filebuf::~Filebuf() {
    close(fd);
}

void Filebuf::store(const char* data, int size) {
    int count = _buf_write(data, size);
    size -= count;
    if (size > 0) [[unlikely]] {
        _file_write(data + count, size);
    }
}

int Filebuf::fetch(char* result, int max_size) {
    int count = _buf_read(result, max_size);
    if (count > 0) [[likely]] {
        return count;
    } else {
        return _file_read(result, max_size);
    }
}

void Filebuf::rewind() {
    next_pos = 0;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        ERROR_EXIT("Cannot lseek");
    }
}

void Filebuf::clear() {
    data_size = 0;
    if (ftruncate(fd, 0) < 0) {
        if (errno == EINTR && ftruncate(fd, 0) == 0) {  // if interrupted
            // pass
        } else {
            ERROR_EXIT("Cannot truncate file '%s'", file_name.c_str());
        }
    }
    rewind();
}

RequestType::Type Filebuf::parse_request_type() {
    if (data_size >= 2) {
        switch (buffer[0]) {
        case 'C':
            if (buffer[1] == 'O')
                return RequestType::CONNECT;
            break;
        case 'D':
            if (buffer[1] == 'E')
                return RequestType::DELETE;
            break;
        case 'G':
            if (buffer[1] == 'E')
                return RequestType::GET;
            break;
        case 'H':
            if (buffer[1] == 'E')
                return RequestType::HEAD;
            break;
        case 'O':
            if (buffer[1] == 'P')
                return RequestType::OPTIONS;
            break;
        case 'T':
            if (buffer[1] == 'R')
                return RequestType::TRACE;
            break;
        case 'P':
            if (buffer[1] == 'O')
                return RequestType::POST;
            else if (buffer[1] == 'U')
                return RequestType::PUT;
            else if (buffer[1] == 'A')
                return RequestType::PATCH;
            else
                break;
        default:
            return RequestType::NONE;
        }
    }
    return RequestType::NONE;
}

int Filebuf::search_double_crlf() {
    buffer[next_pos] = '\0';  // NULL terminates
    char* ptr = strstr(buffer, CRLF CRLF);
    return (ptr) ? ptr - buffer : -1;
}

void Filebuf::search_header_backward(const char* crlf_header_name, char* result) {

}

void Filebuf::search_header_membuf(const char* crlf_header_name, char* result) {
    buffer[next_pos] = '\0';  // NULL terminates
    char* start = strstr(buffer, crlf_header_name);
    if (start == NULL) {
        result[0] = '\0';
        return;
    }
    start += strlen(crlf_header_name);
    char* end = strstr(start, CRLF);
    if (end == NULL) {
        result[0] = '\0';
        return;
    }
    int count = end - start;
    memcpy(result, start, count);
    result[count] = '\0';
}

inline int Filebuf::_buf_remaining_space() {
    if (next_pos < MAX_HEADER_SIZE) [[likely]] {
        return MAX_HEADER_SIZE - next_pos;
    } else {
        return 0;
    }
}

inline int Filebuf::_buf_unread_data_size() {
    return min(data_size, MAX_HEADER_SIZE) - next_pos;
}

int Filebuf::_buf_write(const char* data, int size) {
    int space = _buf_remaining_space();
    if (space > 0) [[likely]] {
        size = min(size, space);
        memcpy(buffer + next_pos, data, size);
        next_pos += size;
        data_size += size;
        LOG2("Buffer occupied %d bytes out of %d\n", size, space);
        return size;
    } else {
        return 0;
    }
}

int Filebuf::_buf_read(char* result, int max_size) {
    int size = _buf_unread_data_size();
    if (size > 0) [[likely]] {
        size = min(size, max_size);
        memcpy(result, buffer + next_pos, size);
        next_pos += size;
        LOG2("Buffer read %d bytes\n", size);
        return size;
    } else {
        return 0;
    }
}

void Filebuf::_file_write(const char* data, int size) {
    int remain = size;
    for (int i = 0; i < 10 && remain > 0; i++) {  // try 10 times
        int count = write(fd, data, remain);
        if (count > 0) {
            remain -= count;
        } else if (errno != EAGAIN && errno != EINTR) {  // not blocking nor interrupt
            ERROR("Write failed");
            usleep(100);  // wait for 100us
        }
    }
    if (remain > 0) {
        WARNING("%d bytes could not be written and were lost", remain);
    }
    data_size += size - remain;
    LOG2("Wrote %d bytes to '%s'\n", size - remain, file_name.c_str());
}

inline int Filebuf::_file_read(char* result, int max_size) {
    int count = read(fd, result, max_size);
    if (count < 0) {
        ERROR("Read failed '%s'", file_name.c_str());
    }
    return count;
}

Client::Client(int fd, const struct sockaddr_in& _addr):
    addr{get_host_and_port(_addr)}, server{NULL}, header_len{0}, content_len{0}
{
    read_evt = new_read_event(fd, Client::recv_msg, this);
    write_evt = new_write_event(fd, Client::send_msg, this);
    filebuf = free_filebufs.front();
    free_filebufs.pop();
}

Client::~Client() {
    LOG1("Connection closed: [%s]\n", addr.c_str());
    close(event_get_fd(read_evt));
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    free_filebufs.push(filebuf);
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

void Client::accept_connection(int master_sock, short/*flag*/, void*/*arg*/) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    if (free_filebufs.empty()) {
        WARNING("Max connection <%d> reached", MAX_CONNECTION);
        reply_with_503_unavailable(sock);
        LOG1("Connection closed: [%s]\n", get_host_and_port(addr).c_str());        
        close(sock);
        return;
    }
    Client* client = new Client(sock, addr);
    add_event(client->read_evt);
    LOG1("Connected by [%s]\n", client->addr.c_str());
}

void Client::recv_msg(int fd, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count == 0) {
        // premature close
        if (client->server) {
            client->server->~Server();
        }
        client->~Client();
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

void Client::send_msg(int fd, short/*flag*/, void* arg) {
    auto client = (Client*)arg;
    int count = client->filebuf->fetch(read_buffer, sizeof(read_buffer));
    if (count <= 0) {
        // done replying
        if (client->server) {
            client->server->~Server();
        }
        client->~Client();
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

Server::Server(Client* _client): client{_client}, filebuf{_client->filebuf} {
    int sock = connect_TCP(Server::address, Server::port);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    LOG2("Connected to [SERVER] (active: %d)\n", ++Server::connection_count);
    read_evt = new_read_event(sock, Server::recv_msg, this);
    write_evt = new_write_event(sock, Server::send_msg, this);
}

Server::~Server() {
    LOG2("Connection closed: [SERVER] (active: %d)\n", --Server::connection_count);
    close(event_get_fd(read_evt));
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
}

bool Server::check_response_completed(int last_read_count) {
    // TODO: HEAD method can has content-length, but no body
    // TODO: 304 Not Modified can has transfer-encoding, but no body

    constexpr int digits = log10(MAX_BODY_SIZE) + 3;
    char res[digits];

    // TODO: detect transfer encoding first

    if (last_read_count == 5) return true;
    return false;

    if (content_len == 0) {  // not yet parsed
        filebuf->search_header_membuf(CRLF "Content-Length:", res);
        content_len = atoi(res);
    }
    if (content_len > 0 && filebuf->data_size /*???*/ == content_len) {
        // TODO: new header_len member?
        return true;
    }
    return false;
}

void Server::send_msg(int fd, short/*flag*/, void* arg) {
    auto server = (Server*)arg;
    int count = server->filebuf->fetch(read_buffer, sizeof(read_buffer));
    if (count <= 0) {
        // done forwarding
        del_event(server->write_evt);
        server->filebuf->clear();
        add_event(server->read_evt);
    }
    write(fd, read_buffer, count);
    LOG2("%11d bytes >> [SERVER]\n", count);
}

void Server::recv_msg(int fd, short/*flag*/, void* arg) {
    auto server = (Server*)arg;
    auto client = server->client;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    server->filebuf->store(read_buffer, count);
    LOG2("%11d bytes << [SERVER]\n", count);

    if (count == 0 || server->check_response_completed(count)) {
        // start replying to client
        del_event(server->read_evt);
        server->filebuf->rewind();
        server->~Server();  // destroy server
        client->server = NULL;
        add_event(client->write_evt);
    }
}

void raise_open_file_limit(unsigned long value) {
    struct rlimit lim;

    if (getrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR_EXIT("Cannot getrlimit");
    }
    if (lim.rlim_max < value) {
        ERROR_EXIT("Please raise hard limit of RLIMIT_NOFILE above %lu", value);
    }
    LOG2("Raising RLIMIT_NOFILE: %lu -> %lu\n", lim.rlim_cur, value);
    lim.rlim_cur = value;
    if (setrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR_EXIT("Cannot setrlimit")
    }
}

int reply_with_503_unavailable(int sock) {
    // copy header
    const char* head = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 60\r\n\r\n";
    char* ptr = read_buffer;
    strncpy(ptr, head, sizeof(read_buffer));
    ptr += strlen(head);
    // copy body from file
    int fd = open("utils/503.html", O_RDONLY);   // TODO: test this!
    int n;
    while ((n = read(fd, ptr, sizeof(read_buffer) - (ptr - read_buffer))) > 0) {
        ptr += n;
    }
    close(fd);
    return write(sock, read_buffer, ptr - read_buffer);
}

int passive_TCP(unsigned short port, int qlen) {
    struct sockaddr_in addr;
    int sockFd;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR_EXIT("Cannot create socket");
    }
    if (bind(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ERROR_EXIT("Cannot bind to port %d", port);
    }
    if (listen(sockFd, qlen) < 0) {
        ERROR_EXIT("Cannot listen");
    }
    LOG1("Listening on port %d...\n", port);
    return sockFd;
}

int connect_TCP(const char* host, unsigned short port) {
    struct sockaddr_in addr;
    struct addrinfo* info;
    int sockFd;

    if (getaddrinfo(host, NULL, NULL, &info) == 0) {  // TODO(davidhcefx): async DNS resolution (see libevent doc)
        memcpy(&addr, info->ai_addr, sizeof(addr));
    } else if ((addr.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        ERROR_EXIT("Cannot resolve host addr: %s\n", host);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR_EXIT("Cannot create socket");
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {  // TODO(davidhcefx): non-blocking connect
        ERROR_EXIT("Cannot connect to %s:%d", get_host(addr), get_port(addr));
    }
    // LOG2("Connected to %s:%d\n", get_host(addr), get_port(addr));
    return sockFd;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("SYNOPSIS\n\t%s <server_addr> <server_port> [port]\n", argv[0]);
        printf("\tserver_addr\tThe address of the server to be protected.\n");
        printf("\tserver_port\tThe port of the server to be protected.\n");
        printf("\tport\tThe port of this proxy to be listening on. (default=8080)\n");
        exit(0);
    }
    Server::address = argv[1];
    Server::port = atoi(argv[2]);
    unsigned short port = (argc >= 4) ? atoi(argv[3]) : 8080;

    raise_open_file_limit(MAX_FILE_DES);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_FILEBUF; i++) {
        free_filebufs.push(make_shared<Filebuf>());
    }
    assert(master_sock == 3 && free_filebufs.back()->get_fd() == 3 + MAX_FILEBUF);

    evt_base = event_base_new();
    add_event(new_read_event(master_sock, Client::accept_connection));
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot setup signal");
    }
    if (event_base_dispatch(evt_base) < 0) {  // blocking
        ERROR_EXIT("Cannot dispatch event");
    }
    event_base_foreach_event(evt_base, close_event_connection, NULL);
    event_base_free(evt_base);
    return 0;
}
