#include "helper.h"
#define SERVER_PORT  8787
#define PROXY_PORT   8089


int child_pid = 0;


void run_proxy();

void setup() {
    // fork a child for running proxy
    if ((child_pid = fork()) < 0) {
        ERROR_EXIT("Cannot fork");
    } else if (child_pid == 0) {  // child
        run_proxy();
        exit(0);
    } else {
        return;
    }
}

void teardown() {
    // terminate proxy
    int stat;
    if (kill(child_pid, SIGINT) < 0) ERROR_EXIT("Kill error");
    if (waitpid(child_pid, &stat, 0) < 0) ERROR_EXIT("Waitpid error");
}

void run_proxy() {
    Server::address = "localhost";
    Server::port = SERVER_PORT;
    unsigned short port = PROXY_PORT;

    // occupy fds
    raise_open_file_limit(MAX_FILE_DSC);
    int master_sock = passive_TCP(port);  // fd should be 3
    for (int i = 0; i < MAX_HYBRIDBUF; i++) {
        free_hybridbuf.push(make_shared<Hybridbuf>("hist"));
    }
    assert(free_hybridbuf.back()->get_fd() == 3 + MAX_HYBRIDBUF);

    // setup parser and signal handlers
    HttpParser::init_all_settings();
    if (signal(SIGINT, break_event_loop) == SIG_ERR) {
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

void proxy_set_all_slow_mode() {
    if (kill(child_pid, SIGUSR1) < 0) ERROR_EXIT("Kill error");
    sleep(3);  // 3 seconds should be enough (for timeout = 1s)
}

vector<Connection*> get_all_connections() {
    vector<Connection*> all_conn;
    vector<struct event*> evt_list;
    event_base_foreach_event(evt_base, add_to_list, &evt_list);  // get events

    for (auto evt : evt_list) {
        auto cb = event_get_callback(evt);
        auto cb_arg = event_get_callback_arg(evt);
        auto append_unique = [=](auto conn) {
            for (auto c : all_conn) if (c == conn) return;
            all_conn.push_back(c);
        };
        if (cb == Client::on_readable || cb == Client::on_writable) {
            append_unique(reinterpret_cast<Client*>(cb_arg)->conn);
        } else if (cb == Server::on_readable || cb == Server::on_writable) {
            append_unique(reinterpret_cast<Server*>(cb_arg)->conn);
        }
    }
    return all_conn;
}

bool test_response_to_HEAD() {
    BEGIN("Testing the response to HEAD...");
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    proxy_set_all_slow_mode();
    auto all_conn = get_all_connections();
    ASSERT_EQUAL(1, all_conn.size());  // only has one connection so far

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto conn = all_conn[0];
    auto request = "HEAD / HTTP/1.1\r\n" \
        "Host: www.csie.ncu.edu.tw\r\nUser-Agent: curl\r\nAccept: */*\r\n\r\n";
    conn->parser->last_method = HTTP_HEAD - 1;  // set to a different value
    write(client_sock, request, strlen(request));
    sleep(1);
    ASSERT_EQUAL(HTTP_HEAD, conn->parser->last_method);  // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 301 Moved Permanently\r\n" \
        "Server: nginx\r\nDate: Thu, 29 Apr 2021 14:30:08 GMT\r\n" \
        "Content-Type: text/html\r\nContent-Length: 178\r\n" \
        "Connection: keep-alive\r\nLocation: https://www.csie.ncu.edu.tw/\r\n\r\n";
    auto dummy = "123";
    conn->parser->set_first_end_of_msg(dummy);  // set to a different value
    write(sock, response, strlen(response));
    sleep(1);
    ASSERT_NOT_EQUAL(dummy, conn->parser->get_first_end_of_msg());  // msg should complete

    END();
}

bool test_304_response() {
    BEGIN("Testing 304 response...");
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    proxy_set_all_slow_mode();
    auto all_conn = get_all_connections();
    auto conn = all_conn[0];

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto request = "GET /~nickm/ HTTP/1.1\r\n" \
        "Host: www.wangafu.net\r\nIf-Modified-Since: Mon, 17 Oct 2016 21:10:05 GMT\r\n\r\n";
    write(client_sock, request, strlen(request));
    sleep(1);
    ASSERT_EQUAL(HTTP_GET, conn->parser->last_method);  // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 304 Not Modified\r\n" \
        "Date: Sat, 01 May 2021 21:24:01 GMT\r\nServer: Apache/2.2.15 (Red Hat)\r\n" \
        "Content-Length: 178\r\nETag: \"ba92a0-1dd2-53f15fffd106e\"\r\n" \
        "Connection: keep-alive\r\n\r\n";
    auto dummy = "123";
    conn->parser->set_first_end_of_msg(dummy);  // set to a different value
    write(sock, response, strlen(response));
    sleep(1);
    ASSERT_NOT_EQUAL(dummy, conn->parser->get_first_end_of_msg());  // msg should complete

    END();
}

int main() {
    Test tests[] = {
        test_response_to_HEAD,
        test_304_response,
    };
    int failed = 0;
    for (auto t : tests) {
        setup();
        if (!t()) {
            failed++;
            printf("\n%18s^^ THIS TEST FAILED!! ^^%18s\n\n", DASH30, DASH30);
        }
        teardown();
    }

    printf("\n" DASH30 DASH30);
    if (failed > 0) {
        printf("%23s%d TEST FAILED!", "", failed);
    } else {
        printf("%24sALL TEST PASSED.\n", "");
    }
    printf(DASH30 DASH30 "\n\n");

    return 0;
}
