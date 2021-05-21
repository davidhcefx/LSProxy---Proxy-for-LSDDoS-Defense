#include "helper.h"


int parent_inbox[2];
int child_inbox[2];


// child_inbox --command--> [execute] --'O'/'X'--> parent_inbox
void run_command_from_parent(int/*sig*/) {
    char command[1024];
    read_until(';', child_inbox[PIPE_R], command, sizeof(command));
    LOG1("%s\n", command);
    auto result = execute_command(command) ? "O" : "X";
    write_with_assert(parent_inbox[PIPE_W], result, 1);
}

void run_proxy(unsigned short port, const char* server_addr, \
               unsigned short server_port) {
    Server::address = server_addr;
    Server::port = server_port;

    // occupy fds
    raise_open_file_limit(MAX_FILE_DSC);
    int master_sock = passive_TCP(port, true);  // fd should be 3
    for (int i = 0; i < MAX_HYBRID_POOL; i++) {
        hybridbuf_pool.push(make_shared<Hybridbuf>("hist"));
    }
    // ASSERT_EQUAL(3 + MAX_HYBRID_POOL, hybridbuf_pool.back()->get_fd());

    // setup parser and signal handlers
    HttpParser::init_all_settings();
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        ERROR_EXIT("Cannot disable SIGPIPE");
    }
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot set SIGINT handler");
    }
    if (signal(SIGUSR1, put_all_connection_slow_mode) == SIG_ERR) {
        ERROR("Cannot set SIGUSR1 handler");
    }
    if (signal(SIGUSR2, run_command_from_parent) == SIG_ERR) {
        ERROR("Cannot set SIGUSR2 handler");
    }
    // run event loop
    evt_base = event_base_new();
    add_event(new_read_event(master_sock, Connection::accept_new));
    if (event_base_dispatch(evt_base) < 0) {  // blocking
        ERROR_EXIT("Cannot dispatch event");
    }
    event_base_foreach_event(evt_base, close_event_fd, NULL);
    event_base_free(evt_base);
}

bool execute_command(string command) {
    static Connection* saved_conn = NULL;
    static const char* saved_first_eom = NULL;
    // command := action [SPACE arg] ;
    command.pop_back();
    stringstream ss(command);
    string action;
    string arg;

    if (ss >> action; action == "save_a_connection") {
        saved_conn = get_first_connection();
        return true;

    } else if (action == "ASSERT_last_method_is") {
        ss >> arg;
        if (saved_conn) {
            if (saved_conn->parser->last_method == method_to_uint8(arg))
                return true;
        }
    } else if (action == "ASSERT_last_method_not") {
        ss >> arg;
        if (saved_conn) {
            if (saved_conn->parser->last_method != method_to_uint8(arg))
                return true;
        }
    } else if (action == "ASSERT_is_fast_mode") {
        if (saved_conn && saved_conn->is_fast_mode()) {
            return true;
        }
    } else if (action == "ASSERT_not_fast_mode") {
        if (saved_conn && !saved_conn->is_fast_mode()) {
            return true;
        }
    } else if (action == "ASSERT_not_in_transition") {
        if (saved_conn && !saved_conn->is_in_transition()) {
            return true;
        }
    } else if (action == "save_first_end_of_msg") {
        if (saved_conn) {
            saved_first_eom = saved_conn->parser->get_first_end_of_msg();
            return true;
        }
    } else if (action == "ASSERT_first_end_of_msg_changed") {
        if (saved_conn) {
            if (saved_conn->parser->get_first_end_of_msg() != saved_first_eom)
                return true;
        }
    } else if (action == "ASSERT_proxy_alive") {
        if (event_base_get_num_events(evt_base, EV_READ | EV_WRITE) > 0)
            return true;

    } else {
        cout << "Invalid action: " << action << endl;
    }

    return false;
}
