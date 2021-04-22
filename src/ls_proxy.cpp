#include "ls_proxy.h"


struct event_base* evt_base;
llhttp_settings_t parser_settings;
char global_buffer[SOCK_IO_BUF_SIZE];  // buffer for each read operation
queue<shared_ptr<Hybridbuf>> free_hybridbuf;
/* class variables */
char* Server::address;
unsigned short Server::port;
unsigned Server::connection_count = 0;


void Connection::do_parse(const char* data, size_t size) {
    auto err = llhttp_execute(parser, data, size);
    if (err != HPE_OK) {
        WARNING("Malformed packet; %s (%s)", parser->reason,
                llhttp_errno_name(err));
        throw ParserError();
    }
}

void Connection::fast_forward(Client*/*client*/, Server*/*server*/) {
    // read to global_buffer for history
    Circularbuf* queue = server->queued_output_f;
    auto stat_c = read_all(client->get_fd(), global_buffer,
                           queue->remaining_space());
    if (stat_c.has_error && !(errno == EAGAIN || errno == EINTR)) {
        ERROR("Read failed");
    }
    try {
        client->keep_track_request_history(global_buffer, stat_c.nbytes);
    } catch (ParserError& err) {
        // close connection, since we can't reliably parse requests
        free_server_and_parser();
        if (client->queued_output_f->data_size() > 0) {
            make_client_reply_only_mode();
        } else {
            delete this;
        }
        return;
    }
    // append to server's queued output, and write them out
    assert(queue->copy_from(global_buffer, stat_c.nbytes) == stat_c.nbytes);
    auto stat_s = queue->write_all_to(server->get_fd());
    LOG2("[%s] %6lu >>>> queue(%lu) >>>> %-6lu [SERVER]\n", client->addr.c_str(),
         stat_c.nbytes, queue->data_size(), stat_s.nbytes);
    if (stat_c.has_eof) {  // client closed
        delete this;
        return;
    }
    if (stat_s.nbytes == 0) {  // server unwritable
        // disable client's read_evt temporarily
        del_event(client->read_evt);
        add_event(server->write_evt);
        LOG2("[%s] Server temporarily unwritable.\n", client->addr.c_str());
    }
}

void Connection::fast_forward(Server*/*server*/, Client*/*client*/) {
    // append to client's queued output, and write them out
    auto stat_s = client->queued_output_f->read_all_from(server->get_fd());
    auto stat_c = client->queued_output_f->write_all_to(client->get_fd());
    LOG2("[%s] %6lu <<<< queue(%lu) <<<< %-6lu [SERVER]\n", client->addr.c_str(),
         stat_c.nbytes, client->queued_output_f->data_size(), stat_s.nbytes);
    if (stat_s.has_eof) {  // server closed
        free_server_and_parser();
        if (client->queued_output_f->data_size() > 0) {
            make_client_reply_only_mode();
        } else {
            delete this;
        }
        return;
    }
    if (stat_c.nbytes == 0) {  // client unwritable
        // disable server's read_evt temporily
        del_event(server->read_evt);
        add_event(client->write_evt);
        LOG2("[%s] Client temporarily unwritable.\n", client->addr.c_str());
    }
}

void Connection::make_client_reply_only_mode() {
    // TODO(davidhcefx): use Filebuf or set timeout to prevent read attack
    del_event(client->read_evt);
    add_event(client->write_evt);
    LOG3("[%s] Client been set to reply-only mode.\n", client->addr.c_str());
}

void Connection::accept_new(int master_sock, short/*flag*/, void*/*arg*/) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    if (free_hybridbuf.empty()) {
        WARNING("Max connection <%d> reached", MAX_CONNECTION);
        reply_with_503_unavailable(sock);
        LOG1("Connection closed: [%s]\n", get_host_and_port(addr).c_str());
        close_socket_gracefully(sock);
        return;
    }
    Connection* conn = new Connection();
    conn->client = new Client(sock, addr, conn);
    add_event(conn->client->read_evt);
    LOG1("Connected by [%s]\n", conn->client->addr.c_str());
}

void close_socket_gracefully(int fd) {
    if (shutdown(fd, SHUT_WR) < 0) ERROR_EXIT("Shutdown error");
    while (read(fd, global_buffer, sizeof(global_buffer)) > 0) {
        // consume input
    }
    if (read(fd, global_buffer, 1) == 0) {  // FIN arrived
        close(fd);
    } else {
        struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
        auto evt = new_read_event(fd, close_after_timeout, event_self_cbarg());
        add_event(evt, &timeout);
    }
}

void close_after_timeout(int fd, short flag, void* arg) {
    auto evt = (struct event*)arg;
    if (flag & EV_TIMEOUT) {
        del_event(evt);
        close(fd);
        LOG3("#%d timed out and were closed.\n", fd);
    } else {
        char buf[0x20];
        while (read(fd, buf, sizeof(buf)) > 0) {}  // consume input
    }
}

struct io_stat read_all(int fd, char* buffer, size_t max_size) {
    struct io_stat stat;
    auto remain = max_size;
    while (remain > 0) {
        auto count = read(fd, buffer, remain);
        if (count > 0) {
            buffer += count;  // move ptr
            remain -= count;
        } else {
            if (count == 0)
                stat.has_eof = true;
            else
                stat.has_error = true;
            break;
        }
    }
    stat.nbytes = max_size - remain;
    return stat;
}

struct io_stat write_all(int fd, const char* data, size_t size) {
    struct io_stat stat;
    auto remain = size;
    while (remain > 0) {
        auto count = write(fd, data, remain);
        if (count > 0) {
            data += count;  // move ptr
            remain -= count;
        } else {
            stat.has_error = true;
            break;
        }
    }
    stat.nbytes = size - remain;
    return stat;
}

void raise_open_file_limit(size_t value) {
    struct rlimit lim;

    if (getrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR_EXIT("Cannot getrlimit");
    }
    if (lim.rlim_max < value) {
        ERROR_EXIT("Please raise hard limit of RLIMIT_NOFILE above %lu", value);
    }
    LOG1("Raising RLIMIT_NOFILE: %lu -> %lu\n", lim.rlim_cur, value);
    lim.rlim_cur = value;
    if (setrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR_EXIT("Cannot setrlimit")
    }
}

size_t reply_with_503_unavailable(int sock) {
    // copy header
    auto head = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 60\r\n\r\n";
    auto count = min(strlen(head), sizeof(global_buffer));
    memcpy(global_buffer, head, count);
    // copy body from file
    int fd = open("utils/503.html", O_RDONLY);
    auto stat = read_all(fd, global_buffer + count,
                         sizeof(global_buffer) - count);
    count += stat.nbytes;
    close(fd);
    LOG3("Replying 503 unavailable to #%d...\n", sock);
    return write_all(sock, global_buffer, count).nbytes;
}

int passive_TCP(unsigned short port, int qlen) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = INADDR_ANY},
        .sin_zero = {0},
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
        ERROR("Cannot resolve host addr: %s", host);
        return -1;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR_EXIT("Cannot create socket");
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {  // TODO(davidhcefx): non-blocking connect
        ERROR("Cannot connect to %s:%d", get_host(addr), get_port(addr));
        return -1;
    }
    // LOG2("Connected to %s:%d\n", get_host(addr), get_port(addr));
    return sockFd;
}

void break_event_loop(int/*sig*/) {
    event_base_loopbreak(evt_base);
}

void put_all_connection_slow_mode(int/*sig*/) {
    event_base_foreach_event(evt_base, put_slow_mode, NULL);
}

int put_slow_mode(const struct event_base*, const struct event* evt, void*) {
    auto cb = event_get_callback(evt);
    auto arg = event_get_callback_arg(evt);

    if (cb == Client::on_readable || cb == Client::on_writable) {
        ((Client*)arg)->conn->set_slow_mode();
    } else if (cb == Server::on_readable || cb == Server::on_writable) {
        ((Server*)arg)->conn->set_slow_mode();
    }  // else can also be other things
}

int close_event_fd(const struct event_base*, const struct event* evt, void*) {
    return close(event_get_fd(evt));
}

int message_complete_cb(llhttp_t* parser, const char* at, size_t/*len*/) {
    parser->data = (void*)at;  // at should points next to end of msg
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("SYNOPSIS\n\t%s <server_addr> <server_port> [port]\n", argv[0]);
        printf("\tserver_addr\tThe address of the server to be protected.\n");
        printf("\tserver_port\tThe port of the server to be protected.\n");
        printf("\tport\tThe port of this proxy to be listening on. (default=8080)\n");
        return 0;
    }
    Server::address = argv[1];
    Server::port = atoi(argv[2]);
    unsigned short port = (argc >= 4) ? atoi(argv[3]) : 8080;

    // occupy fds
    raise_open_file_limit(MAX_FILE_DESC);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_HYBRIDBUF; i++) {
        free_hybridbuf.push(make_shared<Hybridbuf>());
    }
    assert(free_hybridbuf.back()->get_fd() == 3 + MAX_HYBRIDBUF);

    // setup parser
    llhttp_settings_init(&parser_settings);
    parser_settings.on_message_complete = message_complete_cb;
    // setup signal handlers
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot setup SIGINT handler");
    }
    if (signal(SIGUSR1, put_all_connection_slow_mode) == SIG_ERR) {
        ERROR("Cannot setup SIGUSR1 handler");
    }

    // run event loop
    evt_base = event_base_new();
    add_event(new_read_event(master_sock, Connection::accept_new));
    if (event_base_dispatch(evt_base) < 0) {  // blocking
        ERROR_EXIT("Cannot dispatch event");
    }
    event_base_foreach_event(evt_base, close_event_fd, NULL);
    event_base_free(evt_base);
    return 0;
}
