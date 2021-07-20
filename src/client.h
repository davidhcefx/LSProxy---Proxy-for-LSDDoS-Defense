#ifndef SRC_CLIENT_H_
#define SRC_CLIENT_H_
#include "ls_proxy.h"


/**********************************************************
 * Class for handling client I/O.                         *
 **********************************************************/
class Client {
 public:
    string addr;
    struct event* read_evt;  // both events have ptr keeping track of *this
    struct event* write_evt;
    Connection* conn;
    Circularbuf* queued_output;  // queued output for fast-mode
    /* Slow-mode buffers */
    Filebuf* request_buf;        // buffer for a single request
    Filebuf* request_tmp_buf;    // temp buffer for overflow requests
    FIFOfilebuf* response_buf;   // buffer for responses
    /* Counters */
    uint64_t read_count;         // # of bytes read from client
    uint64_t write_count;        // # of bytes write to client

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
        del_event(read_evt);
        add_event(write_evt);
        LOG3("[%s] Client been set to reply-only mode.\n", c_addr());
    }
    // pause reading from client
    void pause_r() { del_event(read_evt); LOG3("[%s] Paused.\n", c_addr()); }
    void resume_r() { add_event(read_evt); LOG3("[%s] Resumed.\n", c_addr()); }
    // keep track of incomplete requests; throw ParserError
    void keep_track_request_history(const char* data, size_t size);
    void copy_history_to(Filebuf* filebuf) {
        Filebuf::copy_data(request_history.get(), filebuf);
    }
    void free_queued_output() {
        if (queued_output) { delete queued_output; queued_output = NULL; }
    }
    void release_request_history() {
        if (request_history && hybridbuf_pool.size() < MAX_HYBRID_POOL) {
            request_history->clear();
            hybridbuf_pool.push(request_history);
        }
        request_history.reset();
    }
    static void on_readable(int fd, short/*flag*/, void* arg);
    static void on_writable(int fd, short/*flag*/, void* arg);

 private:
    shared_ptr<Hybridbuf> request_history;
};

#endif  // SRC_CLIENT_H_
