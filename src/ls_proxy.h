#ifndef SRC_LS_PROXY_H_
#define SRC_LS_PROXY_H_
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <event2/event.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_set>
#include <algorithm>
#include "llhttp/llhttp.h"
#define MAX_CONNECTION    65536      // adjust according to your needs/resources
#define MAX_HYBRID_POOL   MAX_CONNECTION / 4  // # of pre-allocated Hybridbuf
#define SOCK_IO_BUF_SIZE  10 * 1024  // socket io buffer size (B)
#define HIST_CACHE_SIZE   8 * 1024   // in-mem cache for request history (B)
#define SOCK_CLOSE_WAITTIME 30       // the timeout before leaving FIN-WAIT-2
#define MONITOR_INTERVAL    10  // the frequency of monitoring transfer rate (s)
#define TRANSFER_RATE_THRES 1000     // transfer rate threshold (B/s)
#define TRANS_TIMEOUT       4   // timeout before slow-mode transition finish
#define LOG_LEVEL_1                  // minimal info
#define LOG_LEVEL_2                  // abundant info
#define LOG_LEVEL_3                  // very verbose (comment out to disable)
/* Don't modify below this line */
#define MAX_FILE_DSC      7 * MAX_CONNECTION + 7  // see FILE_DESCRIPTORS
#define MAX_REQUEST_SIZE  UINT64_MAX              // currently no enforcement
#define MAX_RESPONSE_SIZE UINT64_MAX
#define __BLANK_10        "          "
#ifdef  LOG_LEVEL_1
#define LOG1(fmt, ...) \
    { printf("%-10lu |" fmt, time(NULL) __VA_OPT__(,) __VA_ARGS__); }
#else
#define LOG1(...)         {}
#endif  // LOG_LEVEL_1
#ifdef  LOG_LEVEL_2
#define LOG2(...)         { printf(__BLANK_10 " |" __VA_ARGS__); }
#else
#define LOG2(...)         {}
#endif  // LOG_LEVEL_2
#ifdef  LOG_LEVEL_3
#define LOG3(...)         { printf(__BLANK_10 " |" __VA_ARGS__); }
#else
#define LOG3(...)         {}
#endif  // LOG_LEVEL_3
#define WARNING(fmt, ...) \
    { fprintf(stderr, "[Warning] " fmt "\n" __VA_OPT__(,) __VA_ARGS__); }
#define ERROR(fmt, ...)   \
    { fprintf(stderr, "[Error] " fmt ": %s (%s:%d)\n" __VA_OPT__(,) \
              __VA_ARGS__, strerror(errno), __FILE__, __LINE__); }
#define ERROR_EXIT(...)   { ERROR(__VA_ARGS__); abort(); }
using std::string;
using std::to_string;
using std::shared_ptr;
using std::make_shared;
using std::vector;
using std::queue;
using std::unordered_set;
using std::exception;
using std::min;
using std::swap;

/**
 * SPECIFICATION:
 * [Init] Every connection initially starts in fast-mode. It can be put into
 *     slow-mode if we found it suspicious, but no way to reverse.
 * [Fast mode] In fast-mode, we forward packets directly. A buffer might be used
 *     for storing temporary data that cannot be forwarded.
 * [Slow mode] In slow-mode, we forward requests ONE AT A TIME and close server
 *     connection after each response. We receive the entire response before
 *     sending back to prevent LSDDoS read attacks.
 *     > Instead of closing it right away, we can maintain a free-server pool
 *       for speedup, but that's complicated.
 *     > Use event-based architecture to prevent proxy itself from LSDDoS.
 *     > Reduce memory usage so that we can endure a lot of slow connections.
 * [Monitor] We monitor transfer-rate periodically, either in every certain
 *     amount of time or when certain amount of bytes received. (The period
 *     should be short, for it opens up a window for DoS attack)
 *     > [Future work] Monitor more metrics for better accuracy.
 * [Transition] When we find a connection suspicious, we first stop collecting
 *     further requests, then close server connection after a short delay in
 *     case there are remaining responses. After switching to slow-mode, we
 *     continue our progress, i.e. if client is requesting, retrieve its
 *     previous packets; if server is responsing, buffer the entire payload
 *     first as stated above.
 * [History] We keep a temporary copy for each incomplete requests, and clear
 *     them once each request completed.
 * [Timeout] To prevent legitimate connections from been recognized as suspicious,
 *     we set keep-alive timeout short and close client connection if timeout.
 *
 * FILE_DESCRIPTORS: (n=MAX_CONNECTION)
 *   stdin + stdout + stderr + master_sock           4
 *   event_base use around 3                         3
 *   Hybridbuf for history (0-1 per connection)      n
 *   Client & Server sockets (1-2 per conn)         2n
 *   Filebuf for slow buffers (0-4 per conn)        4n
 *   Total:                                     7n + 7
 *
 * PROGRESS:
 * [x] Event-based arch.
 * [x] Fast-mode
 * [x] History temp
 * [x] Slow-mode + llhttp + req (hybrid) buffer + resp file buffer
 * [x] Transfer rate monitor
 * [x] Transition logic
 * [x] Detect server down
 * [ ] Shorter keep-alive timeout
 * [ ] TLS support
 * [ ] IPv6 support
 * [ ] HTTP/2.0 support
 */


class Filebuf;
class Hybridbuf;
class FIFOfilebuf;
class Circularbuf;
class Client;
class Server;
class Connection;
class ParserError: public exception {};
class ConnectionError: public exception {};

extern struct event_base* evt_base;
extern char global_buffer[SOCK_IO_BUF_SIZE];
extern queue<shared_ptr<Hybridbuf>> hybridbuf_pool;

/* Struct for storing read/write return values */
struct io_stat {
    size_t nbytes;  // number of bytes actually read/written
    bool has_error;
    bool has_eof;   // when read returns 0
    io_stat(): nbytes{0}, has_error{false}, has_eof{false} {}
};


/* Http request/response parser */
class HttpParser {
 public:
    static llhttp_settings_t request_settings;  // callback settings
    static llhttp_settings_t response_settings;
    uint8_t last_method;  // last request method

    HttpParser():
        last_method{0}, parser{new llhttp_t}, first_eom{NULL}, last_eom{NULL}
    { reset_parser(request_settings); }
    ~HttpParser() { delete parser; }
    // call these only if you are switching between different modes
    void switch_to_request_mode() { reset_parser(request_settings); }
    void switch_to_response_mode() { reset_parser(response_settings); }
    // invoke callbacks along the way; throw ParserError
    void do_parse(const char* data, size_t size);
    bool first_end_of_msg_not_set() { return first_eom == NULL; }
    void set_first_end_of_msg(const char* p) { first_eom = p; }
    void set_last_end_of_msg(const char* p) { last_eom = p; }
    // get nullable pointer to first-encountered end-of-msg (past end)
    const char* get_first_end_of_msg() { return first_eom; }
    const char* get_last_end_of_msg() { return last_eom; }
    static void init_all_settings();

 private:
    llhttp_t* parser;       // (size: ~96 B)
    const char* first_eom;  // first end-of-msg encountered in last run
    const char* last_eom;   // last end-of-msg encountered in last run

    void reset_parser(const llhttp_settings_t& settings) {
        llhttp_init(parser, HTTP_BOTH, &settings);
        parser->data = reinterpret_cast<void*>(this);
    }
};


/******************* Utility functions *******************/
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

// host can be either a hostname or an IP address
inline struct in_addr resolve_host(const char* host) {
    struct addrinfo* info;
    struct in_addr sin_addr;

    // TODO(davidhcefx): async DNS resolution (see libevent doc)
    if (getaddrinfo(host, NULL, NULL, &info) == 0) {
        sin_addr = ((struct sockaddr_in*)info->ai_addr)->sin_addr;
        freeaddrinfo(info);
    } else if ((sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        ERROR_EXIT("Cannot resolve host addr: %s", host);
    }
    return sin_addr;
}

inline ssize_t get_file_size(int fd) {
    struct stat info;
    if (fstat(fd, &info) < 0) [[unlikely]] ERROR_EXIT("Cannot fstat");
    return info.st_size;
}

// raise system-wide RLIMIT_NOFILE
void raise_open_file_limit(rlim_t value);
// read as much as possible
struct io_stat read_all(int fd, char* buffer, size_t max_size);
// write as much as possible
struct io_stat write_all(int fd, const char* data, size_t size);
// reply with contents of 'res/503.html' and return num of bytes written
size_t reply_with_503_unavailable(int sock);
// shutdown write; wait SOCK_CLOSE_WAITTIME seconds (async) for FIN packet
void close_socket_gracefully(int fd);
// timer callback
void close_after_timeout(int/*fd*/, short/*flag*/, void* arg);
// return master socket Fd
int passive_TCP(unsigned short port, bool reuse = false, int backlog = 128);
// return socket Fd
int connect_TCP(const struct sockaddr_in& addr);
// break event_base loop on signal
void break_event_loop(int/*sig*/);
// put current connections to slow-mode on signal
void put_all_connection_slow_mode(int/*sig*/);
// add evt to arg, which is of type vector<struct event*>*
int add_to_list(const struct event_base*, const struct event* evt, void* arg);
// timer callback; scan a list of events and put them to slow-mode
void put_events_slow_mode(int/*fd*/, short/*flag*/, void* arg);
// check transfer rate of all clients, and put suspicious ones to slow-mode
void monitor_transfer_rate(int/*fd*/, short/*flag*/, void*/*arg*/);
// close each event's fd
int close_event_fd(const struct event_base*, const struct event*, void*);
// get associated Connection ptr or NULL
Connection* get_associated_conn(const struct event* evt);

/******************* Parser callbacks ********************/
int headers_complete_cb(llhttp_t* parser);
int message_complete_cb(llhttp_t* parser, const char* at, size_t length);

/******************** Event wrappers *********************/
void del_event(struct event* evt);
void free_event(struct event* evt);

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

inline struct event* new_timer(event_callback_fn cb, vector<const struct event*>* lst) {
    return __new_timer(cb, new struct timer_arg(lst));
}

inline struct event* new_persist_timer(event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, -1, EV_PERSIST, cb, arg);
}

inline void add_event(struct event* evt, const struct timeval* timeout = NULL) {
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

#endif  // SRC_LS_PROXY_H_
