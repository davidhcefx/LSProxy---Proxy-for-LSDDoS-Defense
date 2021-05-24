#ifndef SRC_SERVER_H_
#define SRC_SERVER_H_
#include "ls_proxy.h"


/**********************************************************
 * Class for handling server I/O.                         *
 **********************************************************/
class Server {
 public:
    static struct sockaddr_in address;  // the server to be protected
    static unsigned active_count;       // number of active connections
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
    static bool test_server_alive() {
        if (int sock = connect_TCP(Server::address); sock >= 0) {
            close(sock);
            return true;
        } else {
            WARNING("Server down or having network issue.");
            return false;
        }
    }
};

#endif  // SRC_SERVER_H_
