#ifndef UTILS_SPEED_LIMIT_PROXY_H_
#define UTILS_SPEED_LIMIT_PROXY_H_
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_set>
// Socket io buffer size (Bytes); speed vs. memory.
#define SOCK_IO_BUF_SIZE    10 * 1024
// Redundant define for unused code
#define HIST_CACHE_SIZE     1
// The timeout before leaving TCP FIN-WAIT-2; smaller is nicer to server.
#define SOCK_CLOSE_WAITTIME 10
#define TEMP_FOLDER         "/tmp"
#define LOG1(...)         { printf(__VA_ARGS__); }
#define LOG2(...)         { printf(__VA_ARGS__); }
#define LOG3(...)         { printf(__VA_ARGS__); }
#define WARNING(fmt, ...) \
    { fflush(stdout); \
      fprintf(stderr, "[Warning] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); }
#define ERROR(fmt, ...)   \
    { fflush(stdout); \
      fprintf(stderr, "[Error] " fmt ": %s (%s:%d)\n" __VA_OPT__(,) __VA_ARGS__, \
              strerror(errno), __FILE__, __LINE__); }
#define ERROR_EXIT(...)   { ERROR(__VA_ARGS__); ABORT(); }
#define ABORT()           { abort(); }
using std::string;
using std::to_string;
using std::vector;
using std::unordered_set;
using std::exception;
using std::min;
using std::swap;

/**
 * Proxy for imposing TCP speed limit. That is, for every connection, the
 * I/O stream passing through this proxy would not exceed certain speed limit.
 */


class Circularbuf;
class Client;
class Server;
class Connection;
class ConnectionError: public exception {};
void add_event(struct event* evt, const struct timeval* timeout = NULL);
void del_event(struct event* evt);
void free_event(struct event* evt);

extern struct event_base* evt_base;
extern char global_buffer[SOCK_IO_BUF_SIZE];
extern struct sockaddr_in server_addr;

/* Struct for storing read/write return values */
struct io_stat {
    size_t nbytes;  // number of bytes actually read/written
    bool has_error;
    bool has_eof;   // when read returns 0
    io_stat(): nbytes{0}, has_error{false}, has_eof{false} {}
};


// Class for handling client I/O.
class Client {
 public:
    string addr;
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    Connection* conn;
    Circularbuf* queued_output;

    // create the events
    Client(int fd, const struct sockaddr_in& _addr, Connection* _conn);
    // close socket and delete the events
    ~Client();
    int get_fd() { return event_get_fd(read_evt); }
    const char* c_addr() { return addr.c_str(); }
    void stop_reading() { del_event(read_evt); }
    void stop_writing() { del_event(write_evt); }
    void start_reading() { add_event(read_evt); }
    void start_writing() { add_event(write_evt); }
    static void on_readable(int/*fd*/, short/*flag*/, void* arg);
    static void on_writable(int fd, short/*flag*/, void* arg);
};

// Class for handling server I/O.
class Server {
 public:
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    Connection* conn;
    Circularbuf* queued_output;

    // create the events and connect to server; throw ConnectionError
    explicit Server(Connection* _conn);
    // close socket and delete the events
    ~Server();
    int get_fd() { return event_get_fd(read_evt); }
    void stop_reading() { del_event(read_evt); }
    void stop_writing() { del_event(write_evt); }
    void start_reading() { add_event(read_evt); }
    void start_writing() { add_event(write_evt); }
    static void on_readable(int/*fd*/, short/*flag*/, void* arg);
    static void on_writable(int fd, short/*flag*/, void* arg);
};

// Class for handling client-server interactions.
class Connection {
 public:
    Client* client;
    Server* server;
    uint64_t c2s_count;     // # of bytes sent from client to server
    uint64_t s2c_count;     // # of bytes sent from server to client

    // connects to server; throw ConnectionError
    Connection(int client_fd, const struct sockaddr_in& addr):
        client{new Client(client_fd, addr, this)}, server{new Server(this)},
        c2s_count{0}, s2c_count{0} {}
    ~Connection() {
        delete client;
        delete server;
    }
    // close connection if any of them are closed
    void forward_client_to_server();
    void forward_server_to_client();
    // create Connection and instances
    static void accept_new(int master_sock, short/*flag*/, void*/*arg*/);
};


/******************* Utility functions *******************/
// read as much as possible
struct io_stat read_all(int fd, char* buffer, size_t max_size);
// write as much as possible
struct io_stat write_all(int fd, const char* data, size_t size);

inline char* get_host(const struct sockaddr_in& addr) {
    return inet_ntoa(addr.sin_addr);
}

inline int get_port(const struct sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

// return in format host:port
inline string get_host_and_port(const struct sockaddr_in& addr) {
    return string(get_host(addr)) + ":" + to_string(get_port(addr));
}

// host can be either a hostname (DNS lookup) or an IP address
inline struct in_addr resolve_host(const char* host) {
    struct addrinfo* info;
    struct in_addr sin_addr;

    if (getaddrinfo(host, NULL, NULL, &info) == 0) {  // time consuming
        sin_addr = ((struct sockaddr_in*)info->ai_addr)->sin_addr;
        freeaddrinfo(info);
    } else if ((sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        ERROR_EXIT("Cannot resolve host addr: %s", host);
    }
    return sin_addr;
}

// return master socket Fd
int passive_TCP(unsigned short port, bool reuse = false, int backlog = 128);
// return socket Fd
int connect_TCP(const struct sockaddr_in& addr);
// shutdown write; wait SOCK_CLOSE_WAITTIME seconds (async) for FIN packet
void close_socket_gracefully(int fd);
// timer callback
void close_after_timeout(int/*fd*/, short/*flag*/, void* arg);
// break event_base loop on signal
void break_event_loop(int/*sig*/);
// add evt to arg, which is of type vector<struct event*>*
int add_to_list(const struct event_base*, const struct event* evt, void* arg);
// get associated Connection ptr or NULL
Connection* get_associated_conn(const struct event* evt);
const unordered_set<Connection*>* get_all_connections();

/******************** Event wrappers *********************/
/* Struct for packing two args into one */
struct timer_arg {
    struct event* evt_self;
    union {
        int fd;
        vector<const struct event*>* evt_list;
    };
    explicit timer_arg(int _fd) { fd = _fd; }
    explicit timer_arg(vector<const struct event*>* lst) { evt_list = lst; }
    ~timer_arg() { del_event(evt_self); free_event(evt_self); }
};

inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_READ | EV_PERSIST, cb, arg);
}

inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_WRITE | EV_PERSIST, cb, arg);
}

inline struct event* __new_timer(event_callback_fn cb, struct timer_arg* arg) {
    return (arg->evt_self = evtimer_new(evt_base, cb, arg));
}

// one-shot timer; cb's arg is of type struct timer_arg* (needs to be freed)
inline struct event* new_timer(event_callback_fn cb, int fd) {
    return __new_timer(cb, new struct timer_arg(fd));
}

inline void add_event(struct event* evt, const struct timeval* timeout) {
    if (event_add(evt, timeout) < 0) { [[unlikely]]
        WARNING("event_add failed: %s", strerror(errno));
    }
}

// when called on a pending event, its timeout would be rescheduled
inline void add_event_with_timeout(struct event* evt, int seconds) {
    struct timeval timeout = {.tv_sec = seconds, .tv_usec = 0};
    add_event(evt, &timeout);
}

inline void del_event(struct event* evt) {
    if (event_del(evt) < 0) { [[unlikely]]
        WARNING("event_del failed: %s", strerror(errno));
    }
}

inline void free_event(struct event* evt) { event_free(evt); }

#endif  // UTILS_SPEED_LIMIT_PROXY_H_
