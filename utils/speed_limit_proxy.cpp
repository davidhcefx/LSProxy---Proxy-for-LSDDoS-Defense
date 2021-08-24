#include "speed_limit_proxy.h"
#include "buffer.h"


struct event_base* evt_base;
char global_buffer[SOCK_IO_BUF_SIZE];  // buffer for each read operation
struct sockaddr_in server_addr;


// forward every packets
// record counters (actually written ones)
// resets each counter every 1 second
// > when to close connection? => any of them closed
// > need circularbuf
// we might reach the limits from on_readable or on_writable
// if reached, set a flag and disable read/write event
// keep track the event we disabled, in order to recover


Client::Client(int fd, const struct sockaddr_in& _addr, Connection* _conn):
    addr{get_host_and_port(_addr)}, conn{_conn}, queued_output{new Circularbuf()}
{
    read_evt = new_read_event(fd, Client::on_readable, this);
    write_evt = new_write_event(fd, Client::on_writable, this);
}

Client::~Client() {
    LOG1("[%s] Connection closed.\n", c_addr());
    del_event(read_evt);  // should be called before closing fd
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    close_socket_gracefully(get_fd());
    if (queued_output) delete queued_output;
}

void Client::on_readable(int/*fd*/, short/*flag*/, void* arg) {
    reinterpret_cast<Client*>(arg)->conn->forward_client_to_server();
}

void Client::on_writable(int fd, short/*flag*/, void* arg) {
    // enable server's read because we removed it before
    auto client = reinterpret_cast<Client*>(arg);
    auto conn = client->conn;
    conn->server->start_reading();
    client->stop_writing();
    // write some
    auto stat = client->queued_output->write_all_to(fd);
    LOG2("[%s] %6lu <<<< queue(%lu)\n", client->c_addr(),
         stat.nbytes, client->queued_output->data_size());
}

Server::Server(Connection* _conn):
    conn{_conn}, queued_output{new Circularbuf()}
{
    int sock = connect_TCP(server_addr);
    if (sock < 0) { [[unlikely]]
        WARNING("Server down or having network issue.");
        throw ConnectionError();
    }
    if (evutil_make_socket_nonblocking(sock) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    LOG2("[%15s] Connection created (#%d|#%d)\n", \
         "SERVER", conn->client->get_fd(), sock);
    read_evt = new_read_event(sock, Server::on_readable, this);
    write_evt = new_write_event(sock, Server::on_writable, this);
}

Server::~Server() {
    LOG2("[%15s] Connection closed (#%d)\n", "SERVER", get_fd());
    del_event(read_evt);
    del_event(write_evt);
    free_event(read_evt);
    free_event(write_evt);
    close_socket_gracefully(get_fd());
    if (queued_output) delete queued_output;
}

void Server::on_readable(int/*fd*/, short/*flag*/, void* arg) {
    reinterpret_cast<Client*>(arg)->conn->forward_server_to_client();
}

void Server::on_writable(int fd, short/*flag*/, void* arg) {
    // enable client's read because we removed it before
    auto server = reinterpret_cast<Server*>(arg);
    auto conn = server->conn;
    conn->client->start_reading();
    server->stop_writing();
    // write some
    auto stat = server->queued_output->write_all_to(fd);
    LOG2("[%s] %9s queue(%lu) >>>> %-6lu [SERVER]\n", conn->client->c_addr(),
         "", server->queued_output->data_size(), stat.nbytes);
}

void Connection::forward_client_to_server() {
    // append to server's queued output, and write them out
    auto stat_c = server->queued_output->read_all_from(client->get_fd());
    auto stat_s = server->queued_output->write_all_to(server->get_fd());
    // TODO: where's the limit?
    c2s_count += stat_s.nbytes;
    LOG2("[%s] %6lu >>>> queue(%lu) >>>> %-6lu [SERVER]\n", client->c_addr(),
         stat_c.nbytes, server->queued_output->data_size(), stat_s.nbytes);
    if (stat_c.has_eof) { [[unlikely]]  // client closed
        delete this;
        return;
    }
    if (stat_s.nbytes == 0) { [[unlikely]]  // server unwritable
        // disable client's read temporarily and listen to server's write
        client->stop_reading();
        server->start_writing();
        LOG2("[%s] Server temporarily unwritable.\n", client->c_addr());
    }
}

void Connection::forward_server_to_client() {
    // append to client's queued output, and write them out
    auto stat_s = client->queued_output->read_all_from(server->get_fd());
    auto stat_c = client->queued_output->write_all_to(client->get_fd());
    // TODO
    s2c_count += stat_c.nbytes;
    LOG2("[%s] %6lu <<<< queue(%lu) <<<< %-6lu [SERVER]\n", client->c_addr(),
         stat_c.nbytes, client->queued_output->data_size(), stat_s.nbytes);
    if (stat_s.has_eof) { [[unlikely]]  // server closed
        delete this;
        return;
    }
    if (stat_c.nbytes == 0) { [[unlikely]]  // client unwritable
        // disable server's read temporarily
        server->stop_reading();
        client->start_writing();
        LOG2("[%s] Client temporarily unwritable.\n", client->c_addr());
    }
}

void Connection::accept_new(int master_sock, short/*flag*/, void*/*arg*/) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    try {
        Connection* conn = new Connection(sock, addr);
        conn->client->start_reading();
        conn->server->start_reading();
        LOG1("[%s] Connection created (#%d)\n", conn->client->c_addr(), sock);
    } catch (ConnectionError& err) {
        close(sock);
        return;
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
    int sockFd;
    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR_EXIT("Cannot create socket");
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
        ERROR("Cannot connect to %s:%d", get_host(addr), get_port(addr));
        return -1;
    }
    // LOG2("Connected to %s:%d\n", get_host(addr), get_port(addr));
    return sockFd;
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

int add_to_list(const struct event_base*, const struct event* evt, void* arg) {
    auto evt_list = reinterpret_cast<vector<const struct event*>*>(arg);
    evt_list->push_back(evt);
    return 0;
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

void help() {
    auto print_opt = [](auto name, auto des) {
        printf("  %-22s%s\n", name, des);
    };
    printf("Usage: speed_limit_proxy [OPTIONS] <p_port> <s_host>\n");
    print_opt("p_port", "The port of this proxy.");
    print_opt("s_host", "The host of the server to connect.");

    printf("\nOptions:\n");
    print_opt("-p", "The port of the server. (default=80)");
    print_opt("-s", "The speed that the I/O stream passing through this proxy" \
                    " would not exceed. (B/s, default=10)");
    print_opt("-h", "Display this help message.");
}

int main(int argc, char* argv[]) {
    unsigned short s_port = 80;  // server port
    uint64_t max_speed = 10;
    char c;
    while ((c = getopt(argc, argv, "p:s:h")) > 0) {
        switch (c) {
        case 'p':
            s_port = atoi(optarg);
            break;
        case 's':
            max_speed = atoi(optarg);
            break;
        case 'h':
            help();
            return 1;
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

    server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr = resolve_host(s_host),
        .sin_zero = {0},
    };
    int master_sock = passive_TCP(p_port, true);

    // setup signal handlers
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        ERROR_EXIT("Cannot disable SIGPIPE");
    }
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot set SIGINT handler");
    }

    evt_base = event_base_new();
    add_event(new_read_event(master_sock, Connection::accept_new));
    if (event_base_dispatch(evt_base) < 0) {  /* blocks here */
        ERROR_EXIT("Cannot dispatch event");
    }
    printf("Clearing up...\n");
    for (auto conn : *get_all_connections()) delete conn;
    event_base_free(evt_base);

    return 0;
}
