#ifndef LOW_SLOW_DEFENSE_H_
#define LOW_SLOW_DEFENSE_H_
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
#include <cassert>
#include <string>
#include <memory>
#include <queue>
#define ERRNOSTR            strerror(errno)
#define ERROR(...)          {fprintf(stderr, __VA_ARGS__); exit(1);}
#define MAX_CONNECTIONS     655 // 36     // adjust it if needed
#define MAX_FILEBUF         MAX_CONNECTIONS
#define MAX_REQUEST_BODY    LONG_MAX
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


/* class for using files as buffers */
class Filebuf {
 public:
    int fd;

    Filebuf();
    // guaranteed to write count bytes
    void write(const char* buffer, int count);
    int read(char* buffer, int size);
    void rewind();
    // clear buffer contents
    void clear();

 private:
    int data_size;
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

    // creates an instance and add read event
    static void accept_connection(int master_sock, short flag, void* arg);
    // recv to buffer; fires up connection with server once request completed
    static void recv_msg(int fd, short flag, void* arg);
    // free instance after finished
    static void send_msg(int fd, short flag, void* arg);

    // when it sees CRLF, it sets content_len if request type has body (beware of overflow)

 private:
    // request has body if value >= 100
    enum RequestType { NONE, GET, HEAD, DELETE, CONNECT, OPTIONS, TRACE, POST = 100, PUT, PATCH };
    RequestType req_type;
    int64_t content_len;  // TODO: Transfer-Encoding ??
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
    // send from buffer; clears buffer for response after finished
    static void send_msg(int fd, short flag, void* arg);
    // recv to buffer; response to client after finished
    static void recv_msg(int fd, short flag, void* arg);

    // cut off connection or something to deal with keep-alive

 private:
    shared_ptr<Filebuf> filebuf;  // the same filebuf as client's
};


char* get_host(const struct sockaddr_in& addr) {
    return inet_ntoa(addr.sin_addr);
}

int get_port(const struct sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

string get_host_and_port(const struct sockaddr_in& addr) {
    return string(get_host(addr)) + ":" + to_string(get_port(addr));
}

void raise_open_file_limit(unsigned long value) {
    struct rlimit lim;

    if (getrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR("Cannot getrlimit: %s\n", ERRNOSTR);
    }
    if (lim.rlim_max < value) {
        ERROR("Please raise hard limit of RLIMIT_NOFILE above %lu.\n", value);
    }
    lim.rlim_cur = value;
    if (setrlimit(RLIMIT_NOFILE, &lim) < 0) {
        ERROR("Cannot setrlimit: %s\n", ERRNOSTR)
    }
}

extern char read_buffer[];

// fill filebuf with contents of '503.html'
void init_with_503_file(shared_ptr<Filebuf> filebuf) {
    char* header = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 60\r\n\r\n";
    filebuf.write(header, strlen(header));
    int fd = open("503.html", O_RDONLY);
    int n;
    while ((n = read(fd, read_buffer, sizeof(read_buffer))) > 0) {
        filebuf.write(read_buffer, n);
    }
}

extern struct event_base* evt_base;

inline struct event* new_read_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_READ | EV_PERSIST, cb, arg);
}

inline struct event* new_write_event(int fd, event_callback_fn cb, void* arg = NULL) {
    return event_new(evt_base, fd, EV_WRITE | EV_PERSIST, cb, arg);
}

inline void event_add(struct event* ev) {
    if (event_add(ev, NULL) < 0) {
        ERROR("Cannot add event: %s\n", ERRNOSTR);
    }
}

inline void event_del(struct event* ev) {
    if (event_del(ev) < 0) {
        ERROR("Cannot del event: %s\n", ERRNOSTR);
    }
}

// return master socket Fd
int passiveTCP(unsigned short port, int qlen = 128) {
    struct sockaddr_in addr;
    int sockFd;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Cannot create socket: %s\n", ERRNOSTR);
    }
    if (bind(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ERROR("Cannot bind to port %d: %s\n", port, ERRNOSTR);
    }
    if (listen(sockFd, qlen) < 0) {
        ERROR("Cannot listen: %s\n", ERRNOSTR);
    }
    printf("Listening on port %d...\n", port);
    return sockFd;
}

// return socket Fd; host can be either hostname or IP address.
int connectTCP(const char* host, unsigned short port) {
    struct sockaddr_in addr;
    struct addrinfo* info;
    int sockFd;

    if (getaddrinfo(host, NULL, NULL, &info) == 0) {  // TODO(davidhcefx): change to async DNS resolution
        memcpy(&addr, info->ai_addr, sizeof(addr));
    } else if ((addr.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        ERROR("Cannot resolve host addr: %s\n", host);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Cannot create socket: %s\n", ERRNOSTR);
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {  // TODO(davidhcefx): non-blocking connect
        ERROR("Cannot connect to %s (%d): %s\n", get_host(addr), get_port(addr), ERRNOSTR);
    }
    printf("Connected to %s (%d)\n", get_host(addr), get_port(addr));
    return sockFd;
}

#endif  // LOW_SLOW_DEFENSE_H_
