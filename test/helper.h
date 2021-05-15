#ifndef TEST_HELPER_H_
#define TEST_HELPER_H_
#include "ls_proxy.h"
#define DASH30      "=============================="
/* put the following at the start and the end of each test */
#define BEGIN()     printf("[Running %s]\n", __func__)
#define END()       LOG2("----------Done----------\n"); return true;
#define USE_RET_FALSE_AS_ASSERT
#ifdef  USE_RET_FALSE_AS_ASSERT
#define ASSERT_TRUE(cond)   if (!(cond)) return false;
#else
#define ASSERT_TRUE(cond)   assert(cond);
#endif  // USE_RET_FALSE_AS_ASSERT
#define ASSERT_EQUAL(expect, actual)       ASSERT_TRUE(expect == actual)
#define ASSERT_NOT_EQUAL(nexpect, actual)  ASSERT_TRUE(nexpect != actual)


// type of tests
typedef bool (*Test)();


void display_this_test_failed_msg() {
    printf("\n%18.18s^^ THIS TEST FAILED!! ^^%18.18s\n\n", DASH30, DASH30);
}

void display_tests_summary(int fails) {
    printf(DASH30 DASH30 "\n");

    if (fails > 0) {
        printf("%23s%d TEST FAILED!\n", "", fails);
    } else {
        printf("%24sALL TEST PASSED.\n", "");
    }
    printf(DASH30 DASH30 "\n\n");
}

void write_with_assert(int fd, const char* buffer, size_t size) {
    auto stat = write_all(fd, buffer, size);
    assert(size == stat.nbytes);
}

// return accepted sock fd
int accept_connection(int master_sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    return sock;
}

// get all unique connections
vector<Connection*> get_all_connections() {
    vector<Connection*> conn_list;
    vector<struct event*> evt_list;
    event_base_foreach_event(evt_base, add_to_list, &evt_list);  // get events

    for (auto evt : evt_list) {
        if (auto conn = get_associated_conn(evt)) {
            [&](auto conn) {  // append if not exist
                for (auto c : conn_list) if (c == conn) return;
                conn_list.push_back(conn);
            } (conn);
        }
    }
    return conn_list;
}

// run proxy and block execution
void run_proxy(short port, const char* server_addr, short server_port) {
    Server::address = server_addr;
    Server::port = server_port;

    // occupy fds
    raise_open_file_limit(MAX_FILE_DSC);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_HYBRIDBUF; i++) {
        free_hybridbuf.push(make_shared<Hybridbuf>("hist"));
    }
    assert(free_hybridbuf.back()->get_fd() == 3 + MAX_HYBRIDBUF);

    // setup parser and signal handlers
    HttpParser::init_all_settings();
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        ERROR_EXIT("Cannot disable SIGPIPE");
    }    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
        ERROR_EXIT("Cannot set SIGINT handler");
    }
    if (signal(SIGUSR1, put_all_connection_slow_mode) == SIG_ERR) {
        ERROR("Cannot set SIGUSR1 handler");
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

// compare if the data within the two fd are the same
bool compare_fd_helper(int fd1, int fd2) {
    char local_buffer[sizeof(global_buffer)];
    int n1 = read_all(fd1, global_buffer, sizeof(global_buffer)).nbytes;
    int n2 = read_all(fd2, local_buffer, sizeof(local_buffer)).nbytes;
    ASSERT_EQUAL(n1, n2);
    ASSERT_EQUAL(0, memcmp(global_buffer, local_buffer, n1));
    return true;
}

// assuming msg contained a "Content-Length:" header
bool check_content_length_matched(const string& msg) {
    const char* header = "\r\nContent-Length:";
    const char* crlfcrlf = "\r\n\r\n";

    size_t idx = msg.find(header);
    ASSERT_TRUE(idx > 0);
    idx += strlen(header);
    size_t length = atoi(msg.c_str() + idx);

    idx = msg.find(crlfcrlf, idx);
    ASSERT_TRUE(idx > 0);
    idx += strlen(crlfcrlf);
    ASSERT_EQUAL(length, msg.size() - idx);
    return true;
}

#endif  // TEST_HELPER_H_
