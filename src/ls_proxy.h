#ifndef SRC_LS_PROXY_H_
#define SRC_LS_PROXY_H_
#include "llhttp/llhttp.h"
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <event2/event.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <climits>
#include <cmath>
#include <string>
#include <memory>
#include <queue>
#include <algorithm>
#define MAX_CONNECTION    6  // 65536    // adjust it if needed // TODO: automatically setup
#define MAX_HYBRIDBUF     MAX_CONNECTION
#define MAX_FILE_DESC     5 * MAX_CONNECTION + 3  // see FILE DESCRIPTOR MAP
// #define MAX_BODY_SIZE     ULONG_MAX  // 2^64 B
#define SOCK_IO_BUF_SIZE  10  // 10 * 1024  // socket io buffer (KB)
#define HIST_CACHE_SIZE   8 // 8 * 1024  // in-mem cache for request history (KB)
// #define CRLF              "\r\n"
#define LOG_LEVEL_1
#define LOG_LEVEL_2
#ifdef  LOG_LEVEL_1
#define LOG1(...)         {printf(__VA_ARGS__);}
#else
#define LOG1(...)         {}
#endif  // LOG_LEVEL_1
#ifdef  LOG_LEVEL_2
#define LOG2(...)         {printf(__VA_ARGS__);}
#else
#define LOG2(...)         {}
#endif  // LOG_LEVEL_2
#define WARNING(fmt, ...) {fprintf(stderr, "[Warning] " fmt "\n" __VA_OPT__(,) \
                                   __VA_ARGS__);}
#define ERROR(fmt, ...)   {fprintf(stderr, "[Error] " fmt ": %s\n" __VA_OPT__(,) \
                                   __VA_ARGS__, strerror(errno));}
#define ERROR_EXIT(...)   {ERROR(__VA_ARGS__); exit(1);}
using std::string;
using std::to_string;
using std::shared_ptr;
using std::make_shared;
using std::queue;
using std::min;

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
 *     > Injecting a 'Keep-Alive: timeout' header might prevent TCP reset
 *       issues, but would mess up with the original response.
 *
 * FILE DESCRIPTOR MAP: (n=MAX_CONNECTION)
 *     0 ~ 3    stdin, stdout, stderr, master_sock
 *     4 ~ n+3  Hybridbuf for history (one per connection)
 *   n+4 ~ 3n+3 Client & Server sockets (1-2 per conn)
 *  3n+4 ~ 5n+3 Filebuf for slow buffers (0-2 per conn)
 *
 * Progress:
 * [x] Event-based arch.
 * [ ] Fast-mode (no buffer)
 * [ ] Slow-mode + llhttp + req (hybrid) buffer + resp file buffer
 * [ ] History temp
 * [ ] Transfer rate monitor
 * [ ] Transition logic
 * [-] Shorter keep-alive timeout
 * [-] Detect server down
 */


struct io_stat {
    int nbytes;    // number of bytes actually read/written
    bool has_error;
    bool has_eof;  // when read returns 0
    io_stat(): nbytes{0}, has_error{false}, has_eof{false} {}
};


/* class for using a file as buffer */
class Filebuf {
 public:
    int data_size;

    Filebuf();
    ~Filebuf() {close(fd);}
    // upon disk failure, expect delays or data loss
    // storing before clearing content will cause undefined behavior
    virtual inline void store(const char* data, int size) {_file_write(data, size);}
    // print error message when failed
    virtual inline int fetch(char* result, int max_size) {_file_read(result, max_size);}
    // rewind content cursor (for further reads)
    virtual inline void rewind() {_file_rewind();}
    // clear content and rewind
    void clear();
    inline int get_fd() {return fd;}

 private:
    /* File buffer */
    int fd;
    string file_name;
    // retry 10 times with delays before failure
    void _file_write(const char* data, int size);
    // print error message and return -1 if failed
    inline int _file_read(char* result, int max_size);
    inline void _file_rewind();
};


/* class for using both memory and file as buffer */
class Hybridbuf: public Filebuf {
 public:
    Hybridbuf(): next_pos{0} {};
    ~Hybridbuf() {};
    // @override
    inline void store(const char* data, int size);
    // @override
    inline int fetch(char* result, int max_size);
    // @override
    inline void rewind();

 private:
    /* Memory buffer */
    char buffer[HIST_CACHE_SIZE];
    int next_pos;  // next read/write position, range [0, HIST_CACHE_SIZE]
    inline int _buf_remaining_space();
    inline int _buf_unread_data_size();
    int _buf_write(const char* data, int size);
    int _buf_read(char* result, int max_size);
    inline void _buf_rewind() { next_pos = 0; }
};


/* Efficient in-memory circular buffer */
class Circularbuf {
 public:
    Circularbuf(): start_ptr{0}, end_ptr{0} {}
    // read in as much as possible from fd; set has_eof when eof
    struct io_stat read_all_from(int fd);
    // write out as much as possible to fd
    struct io_stat write_all_to(int fd);
    inline int data_size();
    // return value in range [0, SOCK_IO_BUF_SIZE)
    inline int remaining_space();

 private:
    char buffer[SOCK_IO_BUF_SIZE];  // last byte don't store data
    char* start_ptr;                // points to data start
    char* end_ptr;                  // points next to data end
};


class Connection;

/* class for handling client io */
class Client {
 public:
    string addr;
    Connection* conn;        // TODO: is it worth using shared_ptr?
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    shared_ptr<Hybridbuf> request_hist;  // incomplete request history
    Circularbuf* queued_output_f;  // queued output for fast-mode
    Filebuf* request_buf_s;        // request buffer for slow-mode
    // TODO: we need a queue-like buffer for slow-mode
    // transfer_rate;

    // create the events and allocate Hybridbuf
    Client(int fd, const struct sockaddr_in& _addr, const Connection* _conn);
    // close socket, delete the events and release Hybridbuf
    ~Client();
    int get_fd() {return event_get_fd(read_evt);}
    // upon completed, forward each request to server one at a time
    void recv_to_buffer_slowly(int fd);

    // check if Content-Length or Transfer-Encoding completed
    // if it has neither, check if double CRLF arrived
    bool check_request_completed(int last_read_count);
    // recv to buffer   // TODO
    static void recv_msg(int fd, short/*flag*/, void* arg);
    // free instance after finished
    static void send_msg(int fd, short/*flag*/, void* arg);

 private:
    // unsigned long content_len;  // TODO: Transfer-Encoding ??
};


/* class for handling server io */
class Server {
 public:
    static char* address;         // the server to be protected
    static unsigned short port;   // the server to be protected
    static int connection_count;  // number of active connections
    Connection* conn;
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    Circularbuf* queued_output_f;  // queued output for fast-mode
    Filebuf* response_buf_s;       // response buffer for slow-mode

    // create the events and connect to server
    explicit Server(const Connection* _conn);
    // close socket and delete the events
    ~Server();
    int get_fd() {return event_get_fd(read_evt);}
    // response to client once it completed
    void recv_all_to_buffer(int fd);

    // check for content-length   // TODO: or what?
    bool check_response_completed(int last_read_count);
    // send from buffer; clears buffer for response after finished   // TODO
    static void send_msg(int fd, short/*flag*/, void* arg);
    // recv to buffer
    static void recv_msg(int fd, short/*flag*/, void* arg);

    /* "Connections can be closed at any time"
       "To avoid the TCP reset problem, servers typically close a connection
       in stages.  First, the server performs a half-close by closing only
       the write side of the read/write connection."
    */

 private:
    unsigned long int content_len;  // TODO: Transfer-Encoding ??
};


/* class for handling connections */
class Connection {
 public:
    Client* client;   // TODO: can we change to shared_ptr?
    Server* server;

    Connection();
    ~Connection();
    bool is_fast_mode() {return fast_mode;}
    bool set_slow_mode() {fast_mode = false;}
    // TODO: description
    void fast_forward(Client*/*client*/, Server*/*server*/);
    void fast_forward(Server*/*server*/, Client*/*client*/);
    // create Connection and Client instances
    static void accept_new(int master_sock, short/*flag*/, void*/*arg*/);

 private:
    bool fast_mode;
};


/* Utility functions */
// replace all newlines with dashes
inline char* replace_newlines(char* str);
inline char* get_host(const struct sockaddr_in& addr);
inline int get_port(const struct sockaddr_in& addr);
// return in format host:port
string get_host_and_port(const struct sockaddr_in& addr);
inline int make_file_nonblocking(int fd);
// read as much as possible
struct io_stat read_all(int fd, char* buffer, int max_size);
// write as much as possible
struct io_stat write_all(int fd, const char* data, int size);
// raise system-wide RLIMIT_NOFILE
void raise_open_file_limit(unsigned long value);
// reply with contents of 'res/503.html' and return num of bytes written
int reply_with_503_unavailable(int sock);
// return master socket Fd
int passive_TCP(unsigned short port, int qlen = 128);
// return socket Fd; host can be either hostname or IP address.
int connect_TCP(const char* host, unsigned short port);
// signal handler
void break_event_loop(int/*sig*/) { event_base_loopbreak(evt_base); }
// event_base callback
int close_event_connection(const struct event_base*, const struct event* evt,
                           void*/*arg*/)
{ return close(event_get_fd(evt)); }

/* Event wrappers */
extern struct event_base* evt_base;
inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg = NULL);
inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg = NULL);
inline void add_event(struct event* evt);
inline void del_event(struct event* evt);
inline void free_event(struct event* evt) { event_free(evt); }


#endif  // SRC_LS_PROXY_H_
