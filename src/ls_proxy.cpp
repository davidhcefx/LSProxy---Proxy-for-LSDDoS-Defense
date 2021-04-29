#include "ls_proxy.h"


struct event_base* evt_base;
char global_buffer[SOCK_IO_BUF_SIZE];  // buffer for each read operation
queue<shared_ptr<Hybridbuf>> free_hybridbuf;
/* class variables */
char* Server::address;
unsigned short Server::port;
unsigned Server::connection_count = 0;
llhttp_settings_t HttpParser::request_settings;
llhttp_settings_t HttpParser::response_settings;


// TODO: add [[unlikely]] to where?
// void HeaderWatchdog::watch_this(const char* data, size_t size, bool is_field) {
//     if (has_seen_target) return;
//     if (is_field) {
//         if (!last_call_is_field) {
            /* New field begins */
            // check last one
        //     if (has_found_target()) {
        //         has_seen_target = true;
        //         return;
        //     }
        //     // store current one
        //     if (data[0] != 'C') {
        //         skip_this_line = true;  // skip until next field
        //     } else {
        //         skip_this_line = false;
        //         field_idx = 0;          // rewind
        //         _store_field(data, size);
        //     }
        // } else if (!skip_this_line) {
            /* Continue previous field */
    //         _store_field(data, size);
    //     }
    // } else if (!skip_this_line) {
        /* Called from "on_value" callback */
        // if (last_call_is_field) {
            /* New value begins */
        //     value_idx = 0;             // rewind
        //     _store_value(data, size);
        // } else {
            /* Continue previous value */
//             _store_value(data, size);
//         }
//     }
//     last_call_is_field = is_field;
//     LOG3("Watchdog: Watched %lu bytes.\n", size);
// }

// bool HeaderWatchdog::has_found_target() {
//     const char* target_field = "Connection";
//     const char* target_value = "close";
//     if (has_seen_target || ( \
//             memcmp(field_buf, target_field, strlen(target_field)) == 0 && \
//             memcmp(value_buf, target_value, strlen(target_value)) == 0 \
//             )) {
//         LOG3("Watchdog: Wof Wof! Found target '%s:%s'!\n", target_field, \
//              target_value);
//         return true;
//     }
//     return false;
// }

void HttpParser::do_parse(const char* data, size_t size) {
    first_eom = last_eom = NULL;  // reset end-of-msg pointers
    auto err = llhttp_execute(parser, data, size);
    if (err != HPE_OK) { [[unlikely]]
        WARNING("Malformed packet; %s (%s)", parser->reason,
                llhttp_errno_name(err));
        throw ParserError();
    }
}

void HttpParser::init_all_settings() {
    // request
    llhttp_settings_init(&request_settings);
    request_settings.on_message_complete = message_complete_cb;
    // response
    llhttp_settings_init(&response_settings);
    response_settings.on_headers_complete = headers_complete_cb;
    response_settings.on_message_complete = message_complete_cb;
}

struct io_stat read_all(int fd, char* buffer, size_t max_size) {
    struct io_stat stat;
    auto remain = max_size;
    while (remain > 0) {
        if (auto count = read(fd, buffer, remain); count > 0) {
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
        if (auto count = write(fd, data, remain); count > 0) {
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

void close_socket_gracefully(int fd) {
    if (shutdown(fd, SHUT_WR) < 0) ERROR_EXIT("Shutdown error");
    while (read(fd, global_buffer, sizeof(global_buffer)) > 0) {
        /* consume input */
    }
    if (read(fd, global_buffer, 1) == 0) {  // FIN arrived
        close(fd);
    } else {
        struct timeval timeout = {.tv_sec = SOCK_CLOSE_WAITTIME, .tv_usec = 0};
        auto evt = new_read_event(fd, close_after_timeout, event_self_cbarg());
        add_event(evt, &timeout);
    }
}

void close_after_timeout(int fd, short flag, void* arg) {
    auto evt = reinterpret_cast<struct event*>(arg);
    if (flag & EV_TIMEOUT) {
        del_event(evt);
        close(fd);
        LOG3("#%d timed out and were closed.\n", fd);
    } else {
        char buf[0x20];
        while (read(fd, buf, sizeof(buf)) > 0) {}  // consume input
    }
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

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot create socket");
    }
    if (bind(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot bind to port %d", port);
    }
    if (listen(sockFd, qlen) < 0) { [[unlikely]]
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

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { [[unlikely]]
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
    auto check_and_set_slow_mode = [](auto conn) {
        if (!conn->is_in_transition()) conn->set_slow_mode();
    };

    if (cb == Client::on_readable || cb == Client::on_writable) {
        check_and_set_slow_mode((reinterpret_cast<Client*>(arg))->conn);
    } else if (cb == Server::on_readable || cb == Server::on_writable) {
        check_and_set_slow_mode((reinterpret_cast<Server*>(arg))->conn);
    }
    return 0;
}

int close_event_fd(const struct event_base*, const struct event* evt, void*) {
    return close(event_get_fd(evt));
}

int headers_complete_cb(llhttp_t* parser) {
    auto h_parser = reinterpret_cast<HttpParser*>(parser->data);
    // HEAD or 304 not modified MUST NOT has body
    if (parser->status_code == 304 || h_parser->last_method == HTTP_HEAD) {
        return 1;
    }
    return 0;
}

int message_complete_cb(llhttp_t* parser, const char* at, size_t/*len*/) {
    auto h_parser = reinterpret_cast<HttpParser*>(parser->data);
    // keep track of *at, which points next to the end of message
    if (h_parser->first_end_of_msg_not_set()) {
        h_parser->set_first_end_of_msg(at);
    }
    h_parser->set_last_end_of_msg(at);
    h_parser->last_method = parser->method;  // update
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
    raise_open_file_limit(MAX_FILE_DSC);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_HYBRIDBUF; i++) {
        free_hybridbuf.push(make_shared<Hybridbuf>("hist"));
    }
    assert(free_hybridbuf.back()->get_fd() == 3 + MAX_HYBRIDBUF);

    // setup parser and signal handlers
    HttpParser::init_all_settings();
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot set SIGINT handler");
    }
    if (signal(SIGUSR1, put_all_connection_slow_mode) == SIG_ERR) {
        ERROR("Cannot set SIGUSR1 handler");
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
