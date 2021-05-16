#include "helper.h"
#define SERVER_PORT  8787
#define PROXY_PORT   8089


int child_pid = 0;
int status;

void setup() {
    /* fork a child for running proxy */
    if (pipe(parent_inbox) < 0) ERROR_EXIT("Cannot create pipe");
    if (pipe(child_inbox) < 0) ERROR_EXIT("Cannot create pipe");
    make_fd_nonblocking(parent_inbox[PIPE_R]);

    if ((child_pid = fork()) < 0) {
        ERROR_EXIT("Cannot fork");
    } else if (child_pid == 0) {  // child
        close(parent_inbox[PIPE_R]);
        close(child_inbox[PIPE_W]);
        run_proxy(PROXY_PORT, "localhost", SERVER_PORT);
        exit(0);
    } else {
        close(child_inbox[PIPE_R]);
        close(parent_inbox[PIPE_W]);
        sleep(5);  // wait for startup to complete
    }
}

void teardown() {
    close(parent_inbox[PIPE_R]);
    close(child_inbox[PIPE_W]);
    send_SIGINT_to(child_pid);
    if (waitpid(child_pid, &status, 0) < 0) ERROR_EXIT("Waitpid error");
}

void proxy_set_all_slow_mode() {
    send_SIGUSR1_to(child_pid);
    sleep(3);  // 3 seconds should be enough (for timeout = 1s)
}

void proxy_run(string command) {
    command += ";";
    write_with_assert(child_inbox[PIPE_W], command.c_str(), command.size());
    send_SIGUSR2_to(child_pid);  // trigger command
    sleep(1);
    char res = getchar_with_timeout(parent_inbox[PIPE_R], 5);
    ASSERT_EQUAL('O', res);
}

/******************************************************************************
 * - Client sends a HEAD request.
 * - Server responses a bodiless msg with non-zero Content-Length header.
 * - The proxy should recognize the response has completed.
 ******************************************************************************/
bool test_bodiless_HEAD_response() {
    BEGIN();
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    proxy_set_all_slow_mode();
    proxy_run("save_a_connection");  // keep track this connection

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto request = "HEAD / HTTP/1.1\r\n" \
        "Host: www.csie.ncu.edu.tw\r\nUser-Agent: curl\r\nAccept: */*\r\n\r\n";
    proxy_run("ASSERT_last_method_not HEAD");  // ensure not HEAD
    write_with_assert(client_sock, request, strlen(request));
    sleep(2);
    proxy_run("ASSERT_last_method_is HEAD");   // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 301 Moved Permanently\r\n" \
        "Server: nginx\r\nDate: Thu, 29 Apr 2021 14:30:08 GMT\r\n" \
        "Content-Type: text/html\r\nContent-Length: 178\r\n" \
        "Connection: keep-alive\r\nLocation: https://www.csie.ncu.edu.tw/\r\n\r\n";
    proxy_run("save_first_end_of_msg");
    write_with_assert(sock, response, strlen(response));
    sleep(2);
    proxy_run("ASSERT_first_end_of_msg_changed");  // msg should complete

    END();
}

/******************************************************************************
 * - A 304 response should has no body, even though it may have non-zero
 *   Content-Length header.
 ******************************************************************************/
bool test_bodiless_304_response() {
    BEGIN();
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    proxy_set_all_slow_mode();
    proxy_run("save_a_connection");  // keep track this connection

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto request = "GET /~nickm/ HTTP/1.1\r\n" \
        "Host: www.wangafu.net\r\n" \
        "If-Modified-Since: Mon, 17 Oct 2016 21:10:05 GMT\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(2);
    proxy_run("ASSERT_last_method_is GET");  // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 304 Not Modified\r\n" \
        "Date: Sat, 01 May 2021 21:24:01 GMT\r\nServer: Apache/2.2.15 (Red Hat)\r\n" \
        "Content-Length: 178\r\nETag: \"ba92a0-1dd2-53f15fffd106e\"\r\n" \
        "Connection: keep-alive\r\n\r\n";
    proxy_run("save_first_end_of_msg");
    write_with_assert(sock, response, strlen(response));
    sleep(2);
    proxy_run("ASSERT_first_end_of_msg_changed");  // msg should complete

    END();
}

int main() {
    if (signal(SIGPIPE, [](int){abort();}) == SIG_ERR) {
        ERROR_EXIT("Cannot setup SIGPIPE handler");
    }
    Test tests[] = {
        test_bodiless_HEAD_response,
        test_bodiless_304_response,
    };
    int failed = 0;
    for (auto t : tests) {
        setup();
        if (!t()) {
            failed++;
            display_this_test_failed_msg();
        }
        teardown();
    }
    display_tests_summary(failed);

    return 0;
}
