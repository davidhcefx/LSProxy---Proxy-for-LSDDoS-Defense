#ifndef SRC_LOW_SLOW_DEFENSE_H_
#define SRC_LOW_SLOW_DEFENSE_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
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
#define MAX_CONNECTIONS     655  // 36     // adjust it if needed
#define MAX_FILEBUF         MAX_CONNECTIONS
#define MAX_HEADER_SIZE     8 * 1024   // 8 KB
#define MAX_BODY_SIZE       ULONG_MAX  // 2^64 B
#define ERROR(fmt, ...)     {fprintf(stderr, fmt ": %s\n", __VA_ARGS__, strerror(errno));}
#define ERROR_EXIT(...)     {ERROR(__VA_ARGS__); exit(1)}
#define LOG_LEVEL_1
#define LOG_LEVEL_2
#ifdef  LOG_LEVEL_1
#define LOG1(...)           {printf(__VA_ARGS__);}
#else
#define LOG1(...)           {}
#endif  // LOG_LEVEL_1
#ifdef  LOG_LEVEL_2
#define LOG2(...)           {printf(__VA_ARGS__);}
#else
#define LOG2(...)           {}
#endif  // LOG_LEVEL_2
using std::string;
using std::to_string;
using std::shared_ptr;
using std::make_shared;
using std::queue;

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
 * File Descriptors: (n = MAX_CONNECTIONS)
 *  0 ~ 3       stdin/stdout/stderr, master_sock
 *  4 ~ n+3     filebuf
 *  n+4 ~ 3n+3  client/server sockets (each connection can has at most 3 Fds)
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
    int write_count;

    Filebuf();
    ~Filebuf();
    int get_fd() {return fd;}
    // data might be lost upon failure of writing to the file
    void write(const char* buffer, int num);
    // print warning message if failed
    int read(char* buffer, int size);
    // rewind all cursors
    void rewind();
    // clear contents and rewind
    void clear();
    RequestType::Type parse_request_type();
    // search for header in mem buffer; store value as NULL-terminated string
    // crlf_header_name: header name with CRLF prepended
    void search_header_membuf(const char* crlf_header_name, char* result);

 private:
    // memory buffer
    char buffer[MAX_HEADER_SIZE + 1];  // NULL-terminated
    int cur_pos;
    // file buffer (storing overflow data)
    int fd;
    string file_name;
};


class Server;

/* class for processing client requests */
class Client {
 public:
    string addr;
    shared_ptr<struct event> read_evt;
    shared_ptr<struct event> write_evt;
    shared_ptr<Server> server;   // the associated server

    // allocate filebuf
    Client(int fd, const struct sockaddr_in& _addr, shared_ptr<Filebuf> _filebuf = NULL);
    // close socket, remove the events and release filebuf
    ~Client();
    // check CRLF, transfer-encoding or content-length  // TODO: transfer-encoding
    bool check_request_completed(int last_read_count);
    // creates an instance and add read event
    static void accept_connection(int master_sock, short flag, void* arg);
    // recv to buffer; fires up connection with server once request completed
    static void recv_msg(int fd, short flag, void* arg);
    // free instance after finished
    static void send_msg(int fd, short flag, void* arg);

 private:
    RequestType::Type req_type;
    unsigned long int content_len;  // TODO: Transfer-Encoding ??
    shared_ptr<Filebuf> filebuf;
    bool filebuf_from_pool;  // whether filebuf is allocated from pool
};


/* class for processing server responses */
class Server {
 public:
    static char* address;        // the server to be protected
    static unsigned short port;  // the server to be protected
    shared_ptr<struct event> read_evt;
    shared_ptr<struct event> write_evt;
    shared_ptr<Client> client;   // the associated client

    // creates connection
    explicit Server(Client* _client);
    // close socket and remove the events
    ~Server();
    // check for content-length   // TODO: or what?
    bool check_response_completed(int last_read_count);
    // send from buffer; clears buffer for response after finished
    static void send_msg(int fd, short flag, void* arg);
    // recv to buffer; response to client after finished
    static void recv_msg(int fd, short flag, void* arg);

    // TODO: cut off connection or something to deal with keep-alive

 private:
    unsigned long int content_len;  // TODO: Transfer-Encoding ??
    shared_ptr<Filebuf> filebuf;  // the same filebuf as client's
};


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

// fill filebuf with contents of '503.html'
void init_with_503_file(shared_ptr<Filebuf> filebuf);

// raise system-wide RLIMIT_NOFILE
void raise_open_file_limit(unsigned long value);

extern struct event_base* evt_base;
inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_READ | EV_PERSIST, cb, arg);
}

inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_WRITE | EV_PERSIST, cb, arg);
}

inline void event_add(struct event* evt) {
    if (event_add(evt, NULL) < 0) {
        ERROR_EXIT("Cannot add event");
    }
}

inline void event_add(const shared_ptr<struct event>& evt) {
    event_add(evt.get());
}

inline void event_del(struct event* evt) {
    if (event_del(evt) < 0) {
        ERROR_EXIT("Cannot del event");
    }
}

inline void event_del(const shared_ptr<struct event>& evt) {
    event_del(evt.get());
}

// return master socket Fd
int passiveTCP(unsigned short port, int qlen = 128)

// return socket Fd; host can be either hostname or IP address.
int connectTCP(const char* host, unsigned short port);

#endif  // SRC_LOW_SLOW_DEFENSE_H_
