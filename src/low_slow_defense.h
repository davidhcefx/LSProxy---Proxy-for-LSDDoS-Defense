#ifndef SRC_LOW_SLOW_DEFENSE_H_
#define SRC_LOW_SLOW_DEFENSE_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <event2/event.h>
#include <cmath>
#include <cassert>
#include <string>
#include <memory>
#include <queue>
#include <algorithm>
#define MAX_CONNECTION  6  // 65536    // adjust it if needed // TODO: automatically setup
#define MAX_FILEBUF     MAX_CONNECTION
#define MAX_FILE_DES    3 * MAX_CONNECTION + 3
#define MAX_HEADER_SIZE 8  // 8 * 1024   // 8 KB
#define MAX_BODY_SIZE   ULONG_MAX  // 2^64 B
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
 * SPEC:
 * - Buffer incomplete header (by mem/file), and forward it afterwards.
 * - Buffer body. Start forwarding process when almost done receiving it. If the
 *    final part didn't arrive within a timeout, reset the connection with server.
 * - [TODO] read attack???
 * - Detect malicious connection by some rules (eg. Transfer Rate), and then
 *    perform action on it (eg. drop).
 * - Event-based architecture, supporting more than 65535 simultanous connections.
 *
 * File Descriptors: (n = MAX_CONNECTION)
 *  0 ~ 3       stdin, stdout, stderr, master_sock
 *  4 ~ n+3     filebuf (one per connection)
 *  n+4 ~ 3n+3  client & server sockets (1-2 per connection)
 */
/*
    [x] Buffer incomple header
    [ ] Buffer body, start forwarding when almost done
    [ ] Read attack
    [ ] Detection and action
    [x] Event-based arch
*/


namespace RequestType {
    enum Type {  // request has body if and only if value >= 100
        NONE, GET, HEAD, DELETE, CONNECT, OPTIONS, TRACE,
        POST = 100, PUT, PATCH
    };
}

/* class for using memory and file as buffer */
class Filebuf {
 public:
    int data_size;

    Filebuf();
    ~Filebuf();
    // store data; upon disk failure, expect delays or data loss
    void store(const char* data, int size);
    // fetch data; print warning message if file-reading failed
    int fetch(char* result, int max_size);
    // rewind content cursor
    void rewind();
    // clear content and rewind
    void clear();
    int get_fd() {return fd;}
    RequestType::Type parse_request_type();
    // search for header in mem buffer; store result as NULL-terminated string
    // crlf_header_name: header name with CRLF prepended
    void search_header_membuf(const char* crlf_header_name, char* result);

 private:
    /* Memory buffer */
    char buffer[MAX_HEADER_SIZE + 1];  // last byte is for '\0'
    int next_pos;   // next read/write position
    inline int _buf_remaining_space();
    inline int _buf_unread_data_size();
    // return num of bytes written
    int _buf_write(const char* data, int size);
    // return num of bytes read
    int _buf_read(char* result, int max_size);

    /* File buffer (store additional data) */
    int fd;
    string file_name;
    // retry 10 times with delays before failure
    void _file_write(const char* data, int size);
    inline int _file_read(char* result, int max_size);
};


class Server;

/* class for handling client connections */
class Client {
 public:
    string addr;
    struct event* read_evt;  // both have arg keep track of *this
    struct event* write_evt;
    shared_ptr<Filebuf> filebuf;
    Server* server;          // the associated server

    // create events and allocate filebuf
    Client(int fd, const struct sockaddr_in& _addr);
    // close socket, delete events and release filebuf
    ~Client();
    // check CRLF, transfer-encoding or content-length  // TODO: transfer-encoding
    bool check_request_completed(int last_read_count);
    // creates an instance and add read event
    static void accept_connection(int master_sock, short/*flag*/, void*/*arg*/);
    // recv to buffer; fires up connection with server once request completed
    static void recv_msg(int fd, short/*flag*/, void* arg);
    // free instance after finished
    static void send_msg(int fd, short/*flag*/, void* arg);

 private:
    RequestType::Type req_type;
    unsigned long int content_len;  // TODO: Transfer-Encoding ??
};


/* class for handling server connections */
class Server {
 public:
    static char* address;         // the server to be protected
    static unsigned short port;   // the server to be protected
    static int connection_count;  // number of active connections
    struct event* read_evt;       // both have arg keep track of *this
    struct event* write_evt;
    Client* client;               // the associated client

    // creates connection and events
    explicit Server(Client* _client);
    // close socket and delete the events
    ~Server();
    // check for content-length   // TODO: or what?
    bool check_response_completed(int last_read_count);
    // send from buffer; clears buffer for response after finished
    static void send_msg(int fd, short/*flag*/, void* arg);
    // recv to buffer; response to client after finished
    static void recv_msg(int fd, short/*flag*/, void* arg);

    // TODO: cut off connection or something to deal with keep-alive

 private:
    unsigned long int content_len;  // TODO: Transfer-Encoding ??
    shared_ptr<Filebuf> filebuf;  // the same filebuf as client's
};


// replace newlines with dashes
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

// raise system-wide RLIMIT_NOFILE
void raise_open_file_limit(unsigned long value);

// reply with contents of '503.html' and return num of bytes written
int reply_with_503_unavailable(int sock);

extern struct event_base* evt_base;
inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_READ | EV_PERSIST, cb, arg);
}

inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg = NULL) {
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

inline void free_event(struct event* evt) {
    event_free(evt);
}

// return master socket Fd
int passive_TCP(unsigned short port, int qlen = 128);

// return socket Fd; host can be either hostname or IP address.
int connect_TCP(const char* host, unsigned short port);

void break_event_loop(int/*sig*/) {
    event_base_loopbreak(evt_base);
}

int close_event_connection(const struct event_base*/*base*/,
                           const struct event* evt, void*/*arg*/) {
    close(event_get_fd(evt));
    return 0;
}

#endif  // SRC_LOW_SLOW_DEFENSE_H_
