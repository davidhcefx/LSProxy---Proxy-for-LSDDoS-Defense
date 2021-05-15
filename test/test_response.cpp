#include "helper.h"
#define SERVER_PORT  8787
#define PROXY_PORT   8089


int child_pid = 0;


void setup() {
    // fork a child for running proxy
    if ((child_pid = fork()) < 0) {
        ERROR_EXIT("Cannot fork");
    } else if (child_pid == 0) {  // child
        run_proxy(PROXY_PORT, "localhost", SERVER_PORT);
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

void proxy_set_all_slow_mode() {
    if (kill(child_pid, SIGUSR1) < 0) ERROR_EXIT("Kill error");
    sleep(3);  // 3 seconds should be enough (for timeout = 1s)
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
    auto conn_list = get_all_connections();
    auto conn = conn_list[0];
    ASSERT_EQUAL(1, conn_list.size());  // only has one connection so far

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto request = "HEAD / HTTP/1.1\r\n" \
        "Host: www.csie.ncu.edu.tw\r\nUser-Agent: curl\r\nAccept: */*\r\n\r\n";
    conn->parser->last_method = HTTP_HEAD - 1;  // set to a different value
    write_with_assert(client_sock, request, strlen(request));
    sleep(1);
    ASSERT_EQUAL(HTTP_HEAD, conn->parser->last_method);  // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 301 Moved Permanently\r\n" \
        "Server: nginx\r\nDate: Thu, 29 Apr 2021 14:30:08 GMT\r\n" \
        "Content-Type: text/html\r\nContent-Length: 178\r\n" \
        "Connection: keep-alive\r\nLocation: https://www.csie.ncu.edu.tw/\r\n\r\n";
    auto dummy = "123";
    conn->parser->set_first_end_of_msg(dummy);  // set to a different value
    write_with_assert(sock, response, strlen(response));
    sleep(1);
    // msg should complete
    ASSERT_NOT_EQUAL(dummy, conn->parser->get_first_end_of_msg());

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
    auto conn_list = get_all_connections();
    auto conn = conn_list[0];
    ASSERT_EQUAL(1, conn_list.size());  // only has one connection so far

    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto request = "GET /~nickm/ HTTP/1.1\r\n" \
        "Host: www.wangafu.net\r\nIf-Modified-Since: Mon, 17 Oct 2016 21:10:05 GMT\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(1);
    ASSERT_EQUAL(HTTP_GET, conn->parser->last_method);  // msg should complete

    int sock = accept_connection(server_sock);
    auto response = "HTTP/1.1 304 Not Modified\r\n" \
        "Date: Sat, 01 May 2021 21:24:01 GMT\r\nServer: Apache/2.2.15 (Red Hat)\r\n" \
        "Content-Length: 178\r\nETag: \"ba92a0-1dd2-53f15fffd106e\"\r\n" \
        "Connection: keep-alive\r\n\r\n";
    auto dummy = "123";
    conn->parser->set_first_end_of_msg(dummy);  // set to a different value
    write_with_assert(sock, response, strlen(response));
    sleep(1);
    ASSERT_NOT_EQUAL(dummy, conn->parser->get_first_end_of_msg());  // msg should complete

    END();
}

int main() {
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
