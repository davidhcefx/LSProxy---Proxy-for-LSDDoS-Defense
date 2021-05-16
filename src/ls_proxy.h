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
#include <climits>
#include <string>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_set>
#include <algorithm>
#include "llhttp/llhttp.h"
#define MAX_CONNECTION    6  //65536    // adjust it if needed? TODO: automatically setup
#define MAX_HYBRIDBUF     MAX_CONNECTION
#define MAX_FILE_DSC      7 * MAX_CONNECTION + 6  // see FILE_DESCRIPTOR_MAP
#define SOCK_IO_BUF_SIZE  10  //10 * 1024  // socket io buffer size (KB)
#define HIST_CACHE_SIZE   8  //8 * 1024   // in-mem cache for request history (KB)
#define SOCK_CLOSE_WAITTIME 10  //30       // the timeout before leaving FIN-WAIT-2
#define MONITOR_INTERVAL  10  // the frequency of monitoring transfer rate (s)
#define TRANSFER_RATE_THRES 1000     // transfer rate threshold (B/s)
#define TRANS_TIMEOUT     4   // timeout before slow-mode transition finish
// #define MAX_BODY_SIZE     ULONG_MAX  // 2^64 B
#define __BLANK_10        "          "
#define LOG_LEVEL_1       // minimal info
#define LOG_LEVEL_2       // abundant info
#define LOG_LEVEL_3       // very verbose
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
 * FILE_DESCRIPTOR_MAP: (n=MAX_CONNECTION)
 *     0 ~ 3    stdin, stdout, stderr, master_sock
 *     4 ~ n+3  Hybridbuf for history (one per connection)
 *   n+4 ~ n+6  event_base use around 3
 *   n+7 ~ 3n+6 Client & Server sockets (1-2 per conn)
 *  3n+7 ~ 7n+6 Filebuf for slow buffers (0-4 per conn)
 *
 * Progress:
 * [x] Event-based arch.
 * [x] Fast-mode
 * [x] History temp
 * [x] Slow-mode + llhttp + req (hybrid) buffer + resp file buffer
 * [x] Transfer rate monitor
 * [x] Transition logic
 * [-] Shorter keep-alive timeout
 * [x] Detect server down
 */


/* struct for storing read/write return values */
struct io_stat {
    size_t nbytes;  // number of bytes actually read/written
    bool has_error;
    bool has_eof;   // when read returns 0
    io_stat(): nbytes{0}, has_error{false}, has_eof{false} {}
};

class ParserError: public exception {};
class ConnectionError: public exception {};


/* class for using a file as buffer */
class Filebuf {
 public:
    size_t data_size;

    explicit Filebuf(const char* alias = "");
    virtual ~Filebuf() {close(fd);}
    // make sure content was cleared before storing; data might lost upon disk failure
    virtual void store(const char* data, size_t size) {_file_write(data, size);}
    // error msg would be printed if failed
    virtual ssize_t fetch(char* res, size_t max_size) {return _file_read(res, max_size);}
    // rewind content cursor (for further reads); 0 is to the beginning
    virtual void rewind(size_t amount = 0) {_file_rewind(amount);}
    // clear content and rewind
    void clear();
    int get_fd() {return fd;}
    // copy data from source to dest (overwrite dest)
    static void copy_data(Filebuf* source, Filebuf* dest);

 protected:
    int fd;
    string file_name;

    // retry 10 times before data loss
    void _file_write(const char* data, size_t size);
    virtual ssize_t _file_read(char* res, size_t max_size) {
        return _read_and_log(fd, res, max_size);
    }
    // rewind backwards, amt=0 is to the beginning
    virtual void _file_rewind(size_t amt) {
        int whence = (amt == 0) ? SEEK_SET : SEEK_CUR;
        if (lseek(fd, -amt, whence) < 0) { ERROR_EXIT("Cannot lseek"); }
        LOG3("File #%d: Rewinded -%lu %s\n", fd, amt, (amt == 0) ? "(HEAD)" : "");
    }
    // error msg would be printed if failed
    static ssize_t _read_and_log(int fd, char* result, size_t max_size) {
        auto count = read(fd, result, max_size);
        if (count < 0) { [[unlikely]]
            ERROR("Read failed (#%d)", fd);
        }
        LOG2("File #%d: Read %lu bytes.\n", fd, count);
        return count;
    }
};


/* class for using both memory and file as buffer */
class Hybridbuf: public Filebuf {
 public:
    explicit Hybridbuf(const char* alias = ""): Filebuf{alias}, next_pos{0} {};
    void store(const char* data, size_t size) override;
    ssize_t fetch(char* result, size_t max_size) override;
    void rewind(size_t amount = 0) override;

 protected:
    char buffer[HIST_CACHE_SIZE];
    unsigned next_pos;  // next read/write position, range [0, HIST_CACHE_SIZE]

    size_t _buf_remaining_space() {
        return next_pos < sizeof(buffer) ? sizeof(buffer) - next_pos : 0;
    }
    size_t _buf_unread_data_size() {
        return min(data_size, sizeof(buffer)) - next_pos;
    }
    size_t _buf_write(const char* data, size_t size);
    size_t _buf_read(char* result, size_t max_size);
    // rewind backwards, amt=0 is to the beginning
    void _buf_rewind(unsigned amt) {
        LOG3("Mem-buf #%d: Rewinded -%u\n", fd, (amt == 0) ? next_pos : amt);
        next_pos -= (amt == 0) ? next_pos : amt;
    }
};


/* class for file-based FIFO buffer */
class FIFOfilebuf: public Filebuf {
 public:
    explicit FIFOfilebuf(const char* alias = ""): Filebuf{alias} {
        reader_fd = open(file_name.c_str(), O_RDONLY);  // second fd
        if (reader_fd < 0) { [[unlikely]]
            ERROR_EXIT("Cannot open '%s'", file_name.c_str());
        }
        LOG2("#%d = '%s' (reader)\n", reader_fd, file_name.c_str());
    }
    int get_reader_fd() { return reader_fd; }
    /*
     * void store(const char* data, size_t size);
     * - Store to writer fd; make sure content was cleared before storing.
     * - Data might be lost upon disk failure.
     *
     * ssize_t fetch(char* result, size_t max_size);
     * - Fetch from reader fd; return 0 if there are no data available.
     * - Error message would be printed if failed.
     *
     * void rewind(size_t amount = 0);
     * - If amount == 0, rewind both reader & writer fd to the beginning.
     * - Otherwise, only reader fd would be rewinded.
     */
 private:
    int reader_fd;

    ssize_t _file_read(char* result, size_t max_size) override {
        return _read_and_log(reader_fd, result, max_size);
    }
    void _file_rewind(size_t amt) override;
};


/* efficient in-memory circular buffer */
class Circularbuf {
 public:
    Circularbuf(): start_ptr{buffer}, end_ptr{buffer} {}
    // copy to internal buffer at most remaining_space bytes
    size_t copy_from(const char* data, size_t size);
    // read in as much as possible from fd
    struct io_stat read_all_from(int fd);
    // write out as much as possible to fd; never set has_eof
    struct io_stat write_all_to(int fd);
    void dump_to(Filebuf* filebuf);
    size_t data_size() {
        return (start_ptr <= end_ptr) ? \
            end_ptr - start_ptr : sizeof(buffer) - (start_ptr - end_ptr);
    }
    // return value in range [0, SOCK_IO_BUF_SIZE)
    size_t remaining_space() { return sizeof(buffer) - 1 - data_size(); }

 private:
    char buffer[SOCK_IO_BUF_SIZE];  // last byte don't store data
    char* start_ptr;                // points to data start
    char* end_ptr;                  // points next to data end
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


extern char global_buffer[SOCK_IO_BUF_SIZE];
extern queue<shared_ptr<Hybridbuf>> free_hybridbuf;
class Connection;
void add_event(struct event* evt, const struct timeval* timeout = NULL);
void del_event(struct event* evt);
void free_event(struct event* evt);


/* class for handling client io */
class Client {
 public:
    string addr;
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    Connection* conn;
    Circularbuf* queued_output;  // queued output for fast-mode
    size_t recv_count;           // number of bytes received from client
    /* Slow-mode buffers */
    Filebuf* request_buf;        // buffer for a single request
    Filebuf* request_tmp_buf;    // temp buffer for overflow requests
    FIFOfilebuf* response_buf;   // buffer for responses

    // create the events and allocate Hybridbuf
    Client(int fd, const struct sockaddr_in& _addr, Connection* _conn);
    // close socket, delete the events and release Hybridbuf
    ~Client();
    int get_fd() { return event_get_fd(read_evt); }
    const char* c_addr() { return addr.c_str(); }
    // upon a request completed, pause client and interact with server
    void recv_to_buffer_slowly(int fd);
    // disable send event upon finished
    void send_response_slowly(int fd);
    // disable further receiving and only reply msg
    void set_reply_only_mode() {
        // TODO(davidhcefx): use Filebuf or set timeout to prevent read attack
        stop_recv();
        start_send();
        LOG3("[%s] Client been set to reply-only mode.\n", c_addr());
    }
    void pause_rw() { stop_recv(); stop_send(); LOG3("[%s] Paused.\n", c_addr()); }
    void resume_rw() { start_recv(); start_send(); LOG3("[%s] Resumed.\n", c_addr()); }
    // keep track of incomplete requests; throw ParserError
    void keep_track_request_history(const char* data, size_t size);
    void copy_history_to(Filebuf* filebuf) {
        Filebuf::copy_data(request_history.get(), filebuf);
    }
    void free_queued_output() {
        if (queued_output) { delete queued_output; queued_output = NULL; }
    }
    static void on_readable(int fd, short/*flag*/, void* arg);
    static void on_writable(int fd, short/*flag*/, void* arg);

 private:
    shared_ptr<Hybridbuf> request_history;

    void stop_recv() { del_event(read_evt); }
    void stop_send() { del_event(write_evt); }
    void start_recv() { add_event(read_evt); }
    void start_send() { add_event(write_evt); }
};


/* class for handling server io */
class Server {
 public:
    static const char* address;        // the server to be protected
    static unsigned short port;        // the server to be protected
    static unsigned connection_count;  // number of active connections
    struct event* read_evt;   // both events have ptr keeping track of *this
    struct event* write_evt;
    Connection* conn;
    Circularbuf* queued_output;  // queued output for fast-mode

    // create the events and connect to server; throw ConnectionError
    explicit Server(Connection* _conn);
    // close socket and delete the events
    ~Server();
    int get_fd() { return event_get_fd(read_evt); }
    // resume client upon finished
    void recv_to_buffer_slowly(int fd);
    // upon finished, move data back from client->request_tmp_buf
    void send_request_slowly(int fd);
    void free_queued_output() {
        if (queued_output) { delete queued_output; queued_output = NULL; }
    }
    static void on_readable(int fd, short flag, void* arg);
    static void on_writable(int fd, short/*flag*/, void* arg);
};


/* class for handling client-server interactions */
class Connection {
 public:
    Client* client;
    Server* server;
    HttpParser* parser;  // each connection shares a parser

    Connection(): client{NULL}, server{NULL}, parser{NULL},
                  fast_mode{true}, in_transition{false} {}
    ~Connection() { free_server(); free_client(); free_parser(); }
    bool is_fast_mode() { return fast_mode; }
    // start to transition to slow-mode in stages
    void set_slow_mode();
    // if transition to slow-mode still in progress
    bool is_in_transition() { return in_transition; }
    void set_transition_done() {
        in_transition = false;
        LOG2("[%s] Transition done.\n", client->c_addr());
    }
    // forward client's msg to server; close server when client closed
    void fast_forward(Client*/*client*/, Server*/*server*/);
    // forward server's msg to client; put client to reply-only mode when server closed
    void fast_forward(Server*/*server*/, Client*/*client*/);
    void free_client() { if (client) { delete client; client = NULL; } }
    void free_server() { if (server) { delete server; server = NULL; } }
    void free_parser() { if (parser) { delete parser; parser = NULL; } }
    // create Connection and Client instances
    static void accept_new(int master_sock, short/*flag*/, void*/*arg*/);

 private:
    bool fast_mode;
    bool in_transition;
};


/* Utility functions */
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

inline ssize_t get_file_size(int fd) {
    struct stat info;
    if (fstat(fd, &info) < 0) [[unlikely]] ERROR_EXIT("Cannot fstat");
    return info.st_size;
}

// raise system-wide RLIMIT_NOFILE
void raise_open_file_limit(size_t value);
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
int passive_TCP(unsigned short port, bool reuse = false, int qlen = 128);
// return socket Fd; host can be either hostname or IP address
int connect_TCP(const char* host, unsigned short port);
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

/* Parser callbacks */
int headers_complete_cb(llhttp_t* parser);
int message_complete_cb(llhttp_t* parser, const char* at, size_t length);

/* Event wrappers */
extern struct event_base* evt_base;

/* struct for packing two args into one */
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

// get associated Connection ptr or NULL
inline Connection* get_associated_conn(const struct event* evt) {
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

#endif  // SRC_LS_PROXY_H_
