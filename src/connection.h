#ifndef SRC_CONNECTION_H_
#define SRC_CONNECTION_H_
#include "ls_proxy.h"
#include "client.h"


/**********************************************************
 * Class for handling client-server interactions.         *
 **********************************************************/
class Connection {
 public:
    static size_t connection_count;
    Client* client;
    Server* server;
    HttpParser* parser;  // each connection shares a parser

    Connection(): client{NULL}, server{NULL}, parser{NULL}, fast_mode{true},
                  in_transition{false}
    { connection_count++; }
    ~Connection() {
        free_server(); free_client(); free_parser(); connection_count--;
    }
    bool is_fast_mode() { return fast_mode; }
    // start to transition to slow-mode; reduce memory as much as possible
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

#endif  // SRC_CONNECTION_H_
