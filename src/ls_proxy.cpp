#include "ls_proxy.h"
#include "buffer.h"
#include "client.h"
#include "server.h"
#include "connection.h"


struct event_base* evt_base;
char global_buffer[SOCK_IO_BUF_SIZE];  // buffer for each read operation
queue<shared_ptr<Hybridbuf>> hybridbuf_pool;  // pre-allocated Hybridbuf
/* class variables */
size_t Connection::connection_count = 0;
struct sockaddr_in Server::address;
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

int passive_TCP(unsigned short port, bool reuse, int backlog) {
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
    if (listen(sockFd, backlog) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot listen");
    }
    LOG1("Listening on port %d...\n", port);
    return sockFd;
}

int connect_TCP(const struct sockaddr_in& addr) {
    const auto total_local_addr = 255 * (65100 - 4100 + 1);  // |{next_local_addr()}|
    int sockFd;

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot create socket");
    }
    for (int i = 0; i < total_local_addr; i++) {  // linear probing (slow when #conn > 14M)
        auto lc_addr = next_local_addr();
        if (bind(sockFd, (struct sockaddr*)lc_addr, sizeof(*lc_addr)) >= 0) { [[likely]]
            break;
        }
        if (i >= total_local_addr - 1) { [[unlikely]]
            ERROR("bind: Used up all ports!");
            return -1;
        }
    }
    if (connect(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {  // time consuming
        ERROR("Cannot connect to %s:%d", get_host(addr), get_port(addr));
        return -1;
    }
    // LOG2("Connected to %s:%d\n", get_host(addr), get_port(addr));
    return sockFd;
}

const struct sockaddr_in* next_local_addr() {
    static struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr = {htonl(0x7f000001)},  // range: 127.0.0.[1-255]
        .sin_zero = {0},
    };
    static unsigned short port = 4100;    // range: [4100-65100]

    if (++port > 65100) { [[unlikely]]    // advance port
        port = 4100;
        auto ip = addr.sin_addr.s_addr;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        addr.sin_addr.s_addr += (ip == 0xff00007f) ? 2 << 24 : 1 << 24;
#else
        addr.sin_addr.s_addr = (ip == 0x7f0000ff) ? 0x7f000001 : ip + 1;
#endif
    }
    addr.sin_port = htons(port);
    return &addr;
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
            if (event_pending(evt, EV_READ | EV_WRITE, NULL)) { // event exists
                conn->set_slow_mode();
            }
        }
    }
    delete t_arg->evt_list;
    delete t_arg;
}

void monitor_transfer_rate(int/*fd*/, short/*flag*/, void*/*arg*/) {
    for (auto conn : *get_all_connections()) {
        auto recv_count = conn->client->recv_count;
        auto send_count = conn->client->send_count;
        const size_t min_transfer = TRANSFER_RATE_THRES * MONITOR_INTERVAL;
        const size_t min_download = MIN_DOWNLOAD_RATE * MONITOR_INTERVAL;

        // drop download-too-slow connections (if it was not uploading)
        if (send_count < min_download && recv_count < min_download) { [[unlikely]]
            LOG1("[%s] Detected download rate too slow! (%lu)\n", \
                 conn->client->c_addr(), send_count);
            if (conn->client->recv_too_slow) {
                delete conn;
                continue;
            }
            conn->client->recv_too_slow = true;  // provide second chance
        }
        // check transfer rate (upload + download)
        if (conn->is_fast_mode() && recv_count + send_count < min_transfer) { [[unlikely]]
            LOG1("[%s] Detected transfer rate < threshold! (%lu, %lu)\n", \
                 conn->client->c_addr(), recv_count, send_count);
            conn->set_slow_mode();
        }
        // reset counters
        conn->client->recv_count = conn->client->send_count = 0;
    }
}

int close_event_fd(const struct event_base*, const struct event* evt, void*) {
    return close(event_get_fd(evt));
}

Connection* get_associated_conn(const struct event* evt) {
    auto cb = event_get_callback(evt);
    auto arg = event_get_callback_arg(evt);
    if (cb == Client::on_readable || cb == Client::on_writable) {
        return reinterpret_cast<Client*>(arg)->conn;
    } else if (cb == Server::on_readable || cb == Server::on_writable) {
        return reinterpret_cast<Server*>(arg)->conn;
    } else {
        return NULL;
    }
}

const unordered_set<Connection*>* get_all_connections() {
    static vector<struct event*> evt_list;
    static unordered_set<Connection*> conn_list;
    evt_list.clear();
    conn_list.clear();

    event_base_foreach_event(evt_base, add_to_list, &evt_list);
    for (auto evt : evt_list) {
        if (auto conn = get_associated_conn(evt))
            conn_list.insert(conn);
    }
    return &conn_list;
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

void abort_and_dump() {
    // make sure there are enough spaces for core dumps (~130M)
    WARNING("Generating core dumps...");
    if (auto dir = opendir(TEMP_FOLDER)) {
        while (auto ent = readdir(dir)) {
            if (auto n = string(ent->d_name); n.starts_with("ls_proxy_buf")) {
                unlink((string(TEMP_FOLDER) + "/" + n).c_str());
            }
        }
        closedir(dir);
    }
    abort();
}

void help() {
    auto print_opt = [](auto name, auto des) {
        printf("  %-22s%s\n", name, des);
    };
    printf("Usage: ls_proxy [OPTIONS] <p_port> <s_host>\n");
    print_opt("p_port", "The port of this proxy.");
    print_opt("s_host", "The host of the server to be protected.");

    printf("\nOptions:\n");
    print_opt("-s, --s_port <port>", "The port of the server. (default=80)");
    print_opt("-h, --help", "Display this help message.");
}

int main(int argc, char* argv[]) {
    const char* short_opts = "s:h";
    const struct option long_opts[] = {
        {"s_port", 1, NULL, 's'},
        {"help",   0, NULL, 'h'},
    };
    unsigned short s_port = 80;
    char c;
    while ((c = getopt_long(argc, argv, short_opts, long_opts, NULL)) > 0) {
        switch (c) {
        case 's':
            s_port = atoi(optarg);
            break;
        case 'h':
            help();
            return 0;
        default:
            return 1;
        }
    }
    // two more positional arguments
    if (optind + 2 > argc) {
        help();
        return 1;
    }
    unsigned short p_port = atoi(argv[optind++]);
    const char* s_host = argv[optind++];
    Server::address = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr = resolve_host(s_host),
        .sin_zero = {0},
    };
    if (!Server::test_server_alive()) return 1;

    // occupy fds
    raise_open_file_limit(MAX_FILE_DSC);
    int master_sock = passive_TCP(p_port, true);  // fd should be 3
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
    LOG1("Clearing up...\n");
    for (auto conn : *get_all_connections()) delete conn;
    event_base_free(evt_base);

    return 0;
}
