#include "ls_proxy.h"


queue<shared_ptr<Hybridbuf>> free_hybridbuf;
struct event_base* evt_base;
// char read_buffer[10240];  // temporary buffer //TODO: what size is appropriate?
// class variables
char* Server::address;
unsigned short Server::port;
int Server::connection_count = 0;


Connection::Connection(): client{NULL}, server{NULL}, fast_mode{true} {}

Connection::~Connection() {
    if (server) {
        server->~Server();
    }
    if (client) {
        client->~Client();
    }
}

void Connection::accept_new(int master_sock, short/*flag*/, void*/*arg*/) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    // if (free_filebufs.empty()) {
    //     WARNING("Max connection <%d> reached", MAX_CONNECTION);
    //     reply_with_503_unavailable(sock);
    //     LOG1("Connection closed: [%s]\n", get_host_and_port(addr).c_str());
    //     close(sock);
    //     return;
    // }
    Connection* conn = new Connection();
    conn->client = new Client(sock, addr, conn);
    add_event(conn->client->read_evt);
    LOG1("Connected by [%s]\n", conn->client->addr.c_str());
}

void Connection::fast_forward(Client* client, Server* server) {
    client->a
}

inline char* replace_newlines(char* str) {
    for (char* ptr = strchr(str, '\n'); ptr; ptr = strchr(ptr, '\n')) {
        *ptr = '_';
    }
    return str;
}

inline char* get_host(const struct sockaddr_in& addr) {
    return inet_ntoa(addr.sin_addr);
}

inline int get_port(const struct sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

string get_host_and_port(const struct sockaddr_in& addr) {
    return string(get_host(addr)) + ":" + to_string(get_port(addr));
}

inline int make_file_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = INADDR_ANY},
    };
    int sockFd;

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
        freeaddrinfo(info);
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

inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg) {
    return event_new(evt_base, fd, EV_READ | EV_PERSIST, cb, arg);
}

inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg) {
    return event_new(evt_base, fd, EV_WRITE | EV_PERSIST, cb, arg);
}

inline void add_event(struct event* evt) {
    if (event_add(evt, NULL) < 0) {
        ERROR_EXIT("Cannot add event");
    }
}

inline void del_event(struct event* evt) {
    if (event_del(evt) < 0) {
        ERROR_EXIT("Cannot del event");
    }
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

    raise_open_file_limit(MAX_FILE_DESC);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_HYBRIDBUF; i++) {
        free_filebufs.push(make_shared<Filebuf>());
    }
    assert(master_sock == 3 && free_filebufs.back()->get_fd() == 3 + MAX_HYBRIDBUF);

    evt_base = event_base_new();
    add_event(new_read_event(master_sock, Connection::accept_new));
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
