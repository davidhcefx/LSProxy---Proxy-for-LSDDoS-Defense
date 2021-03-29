#include "src/low_slow_defense.h"


queue<shared_ptr<Filebuf>> free_filebufs;
char* Server::address;
unsigned short Server::port;
struct event_base* evt_base;
char read_buffer[10240];     // TODO: what size is appropriate?


Filebuf::Filebuf(): write_count{0}, cur_pos{0} {
    buffer[0] = '\0';
    char name[] = "/tmp/low_slow_buf_XXXXXX";
    if ((fd = mkstemp(name)) < 0) {
        ERROR_EXIT("Cannot mkstemp");
    }
    if (make_file_nonblocking(fd) < 0) {
        ERROR_EXIT("Cannot make file nonblocking");
    }
    file_name = string(name);
    LOG2("filebuf (%d): %s\n", fd, name);
}

Filebuf::~Filebuf() {
    close(fd);
}

void Filebuf::write(const char* buffer, int num) {
    // TODO: new version
    write_count += num;
    // LOG2("Writing %d bytes to %s\n", num, file_name.c_str());

    for (int i = 0; i < 100 && num > 0; i++) {  // try 100 times
        int count = write(fd, buffer, num);
        if (count > 0) {
            num -= count;
        } else if (errno != EAGAIN && errno != EINTR) {  // blocking or interrupt
            ERROR_EXIT("Write failed");  // TODO: don't abort
        }
    }
    if (num > 0) {
        ERROR_EXIT("Write failed");
    }
}

int Filebuf::read(char* buffer, int size) {
    // TODO: new version
    int count = read(fd, buffer, size);
    if (count < 0) {
        ERROR("Read failed (%s)", file_name.c_str());
    }
    return count;
}

void Filebuf::rewind() {
    cur_pos = 0;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        ERROR_EXIT("Cannot lseek");
    }
}

void Filebuf::clear() {
    write_count = 0;
    if (ftruncate(fd, 0) < 0) {
        if (errno == EINTR && ftruncate(fd, 0) == 0) {  // if interrupted
            // pass
        } else {
            ERROR_EXIT("Cannot truncate file %s", file_name.c_str());
        }
    }
    rewind();
}

RequestType::Type Filebuf::parse_request_type() {

}

void Filebuf::search_header_membuf(const char* crlf_header_name, char* result) {
    char* start = strstr(buffer, crlf_header_name);
    if (start == NULL) {
        result[0] = '\0';
        return;
    }
    start += strlen(crlf_header_name);
    char* end = strstr(start, "\r\n");
    if (end == NULL) {
        result[0] = '\0';
        return;
    }
    int count = end - start;
    memcpy(result, start, count);
    result[count] = '\0';
}

Client::Client(int fd, const struct sockaddr_in& _addr, shared_ptr<Filebuf> _filebuf):
    addr{get_host_an gd_port(_addr)}, req_type{RequestType::NONE}, content_len{0},
    filebuf{_filebuf}, filebuf_from_pool{false}
{
    // allocate events
    read_evt.reset(new_read_event(fd, Client::recv_msg, this));
    write_evt.reset(new_write_event(fd, Client::send_msg, this));
    if (!filebuf) {
        filebuf_from_pool = true;
        filebuf = free_filebufs.front();
        free_filebufs.pop();
    }
}

Client::~Client() {
    LOG1("Connection closed: %s\n", addr.c_str());
    close(event_get_fd(read_evt));
    event_del(read_evt);
    event_del(write_evt);
    if (filebuf_from_pool) {
        free_filebufs.push(filebuf);
    }
}

bool Client::check_request_completed(int last_read_count) {
    // if (req_type == RequestType::NONE) {
        // filebuf->parse_request_type();
    // }
    return false;

    // TODO: when it sees CRLF, it sets content_len if request type has body (beware of overflow)
}

void Client::accept_connection(int master_sock, short flag, void* arg) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    if (free_filebufs.empty()) {
        ERROR("Max connection %d reached", MAX_CONNECTIONS);
        shared_ptr<Client> client(new Client(sock, addr, make_shared<Filebuf>()));
        init_with_503_file(client->filebuf);  // reply with 503 unavailable
        event_add(client->write_evt);
    } else {
        shared_ptr<Client> client(new Client(sock, addr));
        event_add(client->read_evt);
        LOG1("Connected by %s\n", client->addr.c_str());
    }
}

void Client::recv_msg(int fd, short flag, void* arg) {
    Client* client = (Client*)arg;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count == 0) {
        // premature close
        if (client->server) {
            client->server->~Server();
        }
        client->~Client();
        return;
    }
    client->filebuf->write(read_buffer, count);
    if (client->check_request_completed(count)) {
        // start forwarding to server
        event_del(client->read_evt);  // disable further reads
        client->filebuf->rewind();
        shared_ptr<Server> server(new Server(client));
        event_add(server->write_evt);
    }
#ifdef LOG_LEVEL_2
    char* str = strndup(read_buffer, min(count, 50));
    LOG2("[%-21s] Client::recv: %s\n", client->addr.c_str(), str);
    free(str);
#endif
}

void Client::send_msg(int fd, short flag, void* arg) {
    Client* client = (Client*)arg;
    int count = client->filebuf->read(read_buffer, sizeof(read_buffer));
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
    char* str = strndup(read_buffer, min(count, 50));
    LOG2("[%-21s] Client::send: %s\n", client->addr.c_str(), str);
    free(str);
#endif
}

Server::Server(Client* _client): client{_client}, filebuf{_client->filebuf} {
    int sock = connectTCP(Server::address, Server::port);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    // allocate events
    read_evt.reset(new_read_event(sock, Server::recv_msg_and_reply, this));
    write_evt.reset(new_write_event(sock, Server::send_msg, this));
}

Server::~Server() {
    LOG2("Connection closed: %s:%hu\n", Server::address, Server::port);
    close(event_get_fd(read_evt));
    event_del(read_evt);
    event_del(write_evt);
    filebuf.reset(NULL);
}

bool Server::check_response_completed(int last_read_count) {
    constexpr int digits = log10(MAX_BODY_SIZE) + 3;
    char res[digits];

    // TODO: detect transfer encoding first
    return false;

    if (content_len == 0) {  // not yet parsed
        filebuf->search_header_membuf("\r\nContent-Length:", res);
        content_len = atoi(res);
    }
    if (content_len > 0 && filebuf->write_count ??? == content_len) {
        // TODO: new header_len member?
        return true;
    }
    return false;
}

void Server::send_msg(int fd, short flag, void* arg) {
    Server* server = (Server*)arg;
    int count = server->filebuf->read(read_buffer, sizeof(read_buffer));
    if (count <= 0) {
        // done forwarding
        event_del(server->write_evt);
        server->filebuf->clear();
        event_add(server->read_evt);
    }
    write(fd, read_buffer, count);
    LOG2("[%-21s] Server::send: %d\n", "-", count);
}

void Server::recv_msg(int fd, short flag, void* arg) {
    Server* server = (Server*)arg;
    int count = read(fd, read_buffer, sizeof(read_buffer));
    server->filebuf->write(read_buffer, count);
    if (count == 0 || server->check_response_completed(count)) {
        // start replying to client
        event_del(server->read_evt);
        server->filebuf->rewind();
        server->~Server();  // destroy server
        event_add(client->write_evt);
    }
    LOG2("[%-21s] Server::recv: %d\n", "-", count);
}

void init_with_503_file(shared_ptr<Filebuf> filebuf) {
    char* header = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 60\r\n\r\n";
    filebuf.write(header, strlen(header));
    int fd = open("503.html", O_RDONLY);
    int n;
    while ((n = read(fd, read_buffer, sizeof(read_buffer))) > 0) {
        filebuf.write(read_buffer, n);
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
    LOG2("Raising RLIMIT_NOFILE: %d -> %d\n", lim.rlim_cur, value);
    lim.rlim_cur = value;
    if (setrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR_EXIT("Cannot setrlimit")
    }
}

int passiveTCP(unsigned short port, int qlen) {
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

int connectTCP(const char* host, unsigned short port) {
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
    LOG2("Connected to %s:%d\n", get_host(addr), get_port(addr));
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
    int master_sock = passiveTCP(port);  // fd should be 3
    for (int i = 0; i < MAX_FILEBUF; i++) {
        free_filebufs.push(make_shared<Filebuf>());
    }
    assert(master_sock == 3 && free_filebufs.back()->get_fd() == FILEBUF_LAST_FD);
    event_add(new_read_event(master_sock, Client::accept_connection));
    if (event_base_dispatch(evt_base) < 0) {
        ERROR_EXIT("Cannot dispatch event");
    }

    return 0;
}
