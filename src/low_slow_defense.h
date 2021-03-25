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
#include <unordered_map>
#define ERRNOSTR            strerror(errno)
#define ERROR(...)          {fprintf(stderr, __VA_ARGS__); exit(1);}
#define MAX_CONNECTIONS     655 //36            // TODO: how much should it be?
#define MAX_FILEBUF         MAX_CONNECTIONS  // each connection can have at most 3 Fds
#define MAX_CLIENTS         MAX_CONNECTIONS
#define MAX_SERVERS         MAX_CONNECTIONS
#define MAX_FILE_DES        4 + MAX_FILEBUF + MAX_CLIENTS + MAX_SERVERS
#define FILEBUF_LAST_FD     MAX_CONNECTIONS + 3  // see below filebuf range
#define MAX_REQUEST_BODY    LONG_MAX
#define IVAL_FILENO         -1
using std::string;
using std::to_string;
using std::shared_ptr;
using std::make_shared;
using std::queue;
using std::unordered_map;

/**
 * SPEC:
 * - Buffer incomplete header (by mem/file), and forward it afterwards.
 * - Buffer body. Start forwarding process when almost done receiving it. If the
 *   final part didn't arrive within a timeout, reset the connection with server.
 * - [TODO] read attack???
 * - Detect malicious connection by some rules (eg. Transfer Rate), and then
 *   perform action on it (eg. drop).
 * - Event-based architecture, supporting more than 65535 simultanous connections.
 *
 * File Descriptors: (n = MAX_CONNECTIONS)
 *  0 ~ 3       stdin/stdout/stderr, master_sock
 *  4 ~ n+3     filebuf
 *  n+4 ~ 3n+3  client/server sockets
 */


/* class for using files as buffers */
class Filebuf {
 public:
    int fd;
    string file_name;

    Filebuf();
    // clear buffer contents
    void clear();
};


/* class for processing client requests */
class Client {
 public:
    string addr;
    int server_fd;  // the associated server

    explicit Client(const struct sockaddr_in& _addr);
    ~Client();
    // fires up connection with server once request completed
    void recv_msg(int fd);
    // creates an instance, adjust active_fds, and return client_fd
    static int accept_connection(int master_sock);
    // close socket, adjust active_fds, and delete the instance
    static void close_connection(int client_fd);

    // when it sees CRLF, it sets content_len if request type has body (beware of overflow)

 private:
    // request has body if value >= 100
    enum RequestType { NONE, GET, HEAD, DELETE, CONNECT, OPTIONS, TRACE, POST = 100, PUT, PATCH };
    RequestType req_type;
    int64_t content_len;  // TODO: Transfer-Encoding ??
    shared_ptr<Filebuf> filebuf;
};


/* class for processing server responses */
class Server {
 public:
    static char* address;        // the server to be protected
    static unsigned short port;  // the server to be protected
    int client_fd;  // the associated client

    explicit Server(int _client_fd);
    void recv_msg_and_reply(int fd);
    // creates an instance, adjust active_fds, and return server_fd
    static int create_connection();
    // close socket, adjust active_fds, and delete the instance
    static void close_connection(int server_fd);

    // cut off connection or something to deal with keep-alive
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
