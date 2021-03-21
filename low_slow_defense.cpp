#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
// #include <iostream>
#include <string>
#include <array>
#define ERRNOSTR            strerror(errno)
#define ERROR(...)          {fprintf(stderr, __VA_ARGS__); exit(1);}
#define MAX_CONNECTIONS     65536             // TODO: how much should it be?
#define MAX_FILE_DES        4 + MAX_CONNECTIONS * 2  // each connection can hold up to two Fd
#define MAX_REQUEST_BODY    LONG_MAX
using std::string;
using std::to_string;
using std::array;

/**
 * SPEC:
 * - Buffer incomplete header (by mem/file), and forward it afterwards.
 * - Buffer body. Start forwarding process when almost done receiving it. If the
 *   final part didn't arrive within a timeout, reset the connection with server.
 * - [TODO] read attack???
 * - Detect malicious connection by some rules (eg. Transfer Rate), and then
 *   perform action on it (eg. drop).
 * - Event-based architecture, supporting more than 65535 simultanous connections.
 */


class Connection {
public:
    string client_addr;

    Connection(int _client_fd, const struct sockaddr_in& addr);
    bool recv();

    // when it sees CRLF, it sets content_len if request type has body (beware of overflow)

private:
    // request has body if value >= 100
    enum RequestType { GET=1, HEAD, DELETE, CONNECT, OPTIONS, TRACE, POST=100, PUT, PATCH };
    int client_fd;
    int server_fd;
    RequestType req_type;
    long long content_len;  // TODO: Transfer-Encoding ??
};

char* server_addr;  // the server to be protected
char read_buffer[10240];  // TODO: what size is appropriate?
array<Connection*, MAX_FILE_DES> connections;  // use client_fd as key


char* get_host(const struct sockaddr_in& addr) {
    return inet_ntoa(addr.sin_addr);
}

int get_port(const struct sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

string get_host_and_port(const struct sockaddr_in& addr) {
    return string(get_host(addr)) + ":" + to_string(get_port(addr));
}

Connection::Connection(int _client_fd, const struct sockaddr_in& addr):
    client_addr{get_host_and_port(addr)}, client_fd{_client_fd}, server_fd{-1}, content_len{0}
{}

// return master socket Fd
int passiveTCP(unsigned short port, int qlen=128) {
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

    if (getaddrinfo(host, NULL, NULL, &info) == 0) {
        memcpy(&addr, info->ai_addr, sizeof(addr));
    } else if ((addr.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
        ERROR("Cannot resolve host addr: %s\n", host);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if ((sockFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Cannot create socket: %s\n", ERRNOSTR);
    }
    if ((connect(sockFd, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
        ERROR("Cannot connect to %s (%d): %s\n", get_host(addr), get_port(addr), ERRNOSTR);
    }
    printf("Connected to %s (%d)\n", get_host(addr), get_port(addr));
    return sockFd;
}

void raise_open_file_limit(long unsigned int value) {
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

// return false if connection closed  // TODO: returns what?
bool handle_msg(int fd) {



    int count = read(fd, read_buffer, sizeof(read_buffer));
    if (count <= 0) {
        return false;
    }

    write(1, read_buffer, count);

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("SYNOPSIS\n\t%s <server_addr> [port]\n", argv[0]);
        printf("\tserver_addr\tThe address of the server to be protected.\n");
        printf("\tport\tThe port of this proxy to be listening on. (default=8080)\n");
        exit(0);
    }
    server_addr = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : 8080;
    int master_sock = passiveTCP(port);
    fd_set active_fds;
    fd_set read_fds;

    raise_open_file_limit(MAX_FILE_DES);
    FD_ZERO(&active_fds);
    FD_SET(master_sock, &active_fds);

    while (true) {
        memcpy(&read_fds, &active_fds, sizeof(read_fds));

        if (select(MAX_FILE_DES, &read_fds, NULL, NULL, NULL) < 0) {
            ERROR("Error in select: %s\n", ERRNOSTR);
        }
        for (int fd = 0; fd < MAX_FILE_DES; fd++) {
            if (FD_ISSET(fd, &read_fds)) {
                if (fd == master_sock) {
                    // new connection
                    struct sockaddr_in addr;
                    socklen_t addr_len = sizeof(addr);
                    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);

                    connections[sock] = new Connection(sock, addr);
                    FD_SET(sock, &active_fds);  // keep track
                    printf("Connected by %s\n", connections[sock]->client_addr.c_str());
                } else {
                    // new message
                    if (!handle_msg(fd)) {
                        // printf("Connection closed by %s\n", connections[fd]->client_addr.c_str());
                        // FD_CLR(fd, &active_fds);
                        // delete connections[fd];
                    }
                }
            }
        }
    }
    return 0;
}


