#include "ls_proxy.h"


struct event_base* evt_base;
char global_buffer[SOCK_IO_BUF_SIZE];  // buffer for each read operation
queue<shared_ptr<Hybridbuf>> hybridbuf_pool;  // pre-allocated Hybridbuf
/* class variables */
size_t Connection::connection_count = 0;
const char* Server::address;
unsigned short Server::port;
unsigned Server::active_count = 0;
llhttp_settings_t HttpParser::request_settings;
llhttp_settings_t HttpParser::response_settings;


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

void raise_open_file_limit(rlim_t value) {
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

size_t reply_with_503_unavailable(int sock) {
    int file_fd = open("utils/503.html", O_RDONLY);
    if (file_fd < 0) [[unlikely]] ERROR_EXIT("Cannot open 'utils/503.html'");
    auto body_size = get_file_size(file_fd);
    string header = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 60\r\n" \
                    "Connection: close\r\nContent-Length: ";
    header += to_string(body_size) + "\r\n\r\n";

    // header
    size_t count = min(header.size(), sizeof(global_buffer));
    memcpy(global_buffer, header.c_str(), count);
    // body
    auto space = sizeof(global_buffer) - count;
    count += read_all(file_fd, global_buffer + count, space).nbytes;
    close(file_fd);
    LOG3("Replying 503 unavailable to #%d...\n", sock);
    return write_all(sock, global_buffer, count).nbytes;
}

void close_socket_gracefully(int fd) {
    if (shutdown(fd, SHUT_WR) < 0) { [[unlikely]]
        WARNING("Shutdown write (#%d): %s", fd, strerror(errno));
    }
    while (read(fd, global_buffer, sizeof(global_buffer)) > 0) {/* consume */}
    if (read(fd, global_buffer, 1) == 0) {  // FIN arrived
        close(fd);
        LOG3("#%d closed.\n", fd);
    } else {
        auto evt = new_timer(close_after_timeout, fd);
        add_event_with_timeout(evt, SOCK_CLOSE_WAITTIME);
        // TODO(davidhcefx): is it worth to use the O(1) queue optimization?
    }
}

void close_after_timeout(int/*fd*/, short/*flag*/, void* arg) {
    auto t_arg = reinterpret_cast<struct timer_arg*>(arg);
    close(t_arg->fd);
    LOG3("#%d closed.\n", t_arg->fd);
    delete t_arg;
}

int passive_TCP(unsigned short port, bool reuse, int qlen) {
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
    if (reuse && evutil_make_listen_socket_reuseable_port(sockFd) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot set socket port reusable");
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
    /* When a signal arrived, the event being processed would be interrupted,
      which could lead to unsafe connection state. So we postpone our action
      by adding a new event. */
    auto evt_list = new vector<const struct event*>;
    event_base_foreach_event(evt_base, add_to_list, evt_list);
    auto e = new_timer(put_events_slow_mode, evt_list);
    add_event_with_timeout(e, 1);  // schedule 1s later
    LOG3("Scheduled %lu events to be inspected.\n", evt_list->size());
}

int add_to_list(const struct event_base*, const struct event* evt, void* arg) {
    auto evt_list = reinterpret_cast<vector<const struct event*>*>(arg);
    evt_list->push_back(evt);
    return 0;
}

void put_events_slow_mode(int/*fd*/, short/*flag*/, void* arg) {
    auto t_arg = reinterpret_cast<struct timer_arg*>(arg);
    for (auto evt : *(t_arg->evt_list)) {
        if (auto conn = get_associated_conn(evt)) {
            if (event_pending(evt, EV_READ | EV_WRITE, NULL) /* event exists */ \
                    && conn->is_fast_mode()          /* not slow-mode */ \
                    && !conn->is_in_transition()) {  /* not transitioning */
                conn->set_slow_mode();
            }
        }
    }
    delete t_arg->evt_list;
    delete t_arg;
}

void monitor_transfer_rate(int/*fd*/, short/*flag*/, void*/*arg*/) {
    static vector<struct event*> evt_list;
    static unordered_set<Connection*> conn_list;
    evt_list.clear();
    conn_list.clear();
    // get unique connections
    event_base_foreach_event(evt_base, add_to_list, &evt_list);
    for (auto evt : evt_list) {
        if (auto conn = get_associated_conn(evt)) conn_list.insert(conn);
    }
    // check transfer rate
    for (auto conn : conn_list) {
        const auto threshold = TRANSFER_RATE_THRES * MONITOR_INTERVAL;
        if (conn->client->recv_count < threshold) {
            LOG1("[%s] Detected transfer rate < threshold! (%lu)\n", \
                 conn->client->c_addr(), conn->client->recv_count);
            // conn->set_slow_mode();
        }
        conn->client->recv_count = 0;  // reset counter
    }
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
    // TODO(davidhcefx): change to some public api instead of accessing method
    //                   or status_code directly.
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
    for (int i = 0; i < MAX_HYBRID_POOL; i++) {
        hybridbuf_pool.push(make_shared<Hybridbuf>("hist"));
    }
    assert(hybridbuf_pool.back()->get_fd() == 3 + MAX_HYBRID_POOL);

    // setup parser and signal handlers
    HttpParser::init_all_settings();
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        ERROR_EXIT("Cannot disable SIGPIPE");
    }
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot set SIGINT handler");
    }
    if (signal(SIGUSR1, put_all_connection_slow_mode) == SIG_ERR) {
        ERROR("Cannot set SIGUSR1 handler");
    }

    evt_base = event_base_new();
    add_event_with_timeout(new_persist_timer(monitor_transfer_rate), MONITOR_INTERVAL);
    add_event(new_read_event(master_sock, Connection::accept_new));
    if (event_base_dispatch(evt_base) < 0) {  /* blocks here */
        ERROR_EXIT("Cannot dispatch event");
    }
    event_base_foreach_event(evt_base, close_event_fd, NULL);
    event_base_free(evt_base);
    return 0;
}
