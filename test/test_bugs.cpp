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

void send_SIGUSR1() {
    if (kill(child_pid, SIGUSR1) < 0) ERROR_EXIT("Kill error");
}

// return child pid
int send_SIGUSR1_background(int milisec) {
    int pid = 0;
    if ((pid = fork()) < 0) {
        ERROR_EXIT("Cannot fork");
    } else if (pid == 0) {
        while (true) {
            send_SIGUSR1();
            usleep(milisec * 1000);
        }
        exit(0);
    } else {
        return;
    }
}

/*********************************************************
 * # Close with RST instead of FIN
 * - server not listening
 * - client (on the same host) sends some message
 * => proxy replied 503 to client, then close connection.
 * => Unable to read the 503 message because of receiving RST.
 *********************************************************/
bool test_close_with_rst_instead_of_fin() {
    BEGIN();
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    auto request = "GET / HTTP/1.1\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(1);

    // get 503 response
    auto stat = read_all(client_sock, global_buffer, sizeof(global_buffer));
    global_buffer[stat.nbytes] = '\0';
    string msg(global_buffer);
    ASSERT_TRUE(check_content_length_matched(msg));

    END();
}

/*********************************************************
 * # Signal not been handled correctly
 * - client connects
 * - fire SIGUSR1
 * - terminate client before callback timeouts
 * => Crash, for calling methods on an non-existence instance.
 *********************************************************/
bool test_signal_not_handled_correctly() {
    BEGIN();
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    auto conn_list = get_all_connections();
    auto conn = conn_list[0];
    ASSERT_EQUAL(1, conn_list.size());  // only has one connection so far

    send_SIGUSR1();
    ASSERT_EQUAL(false, conn->is_in_transition());  // not fired yet
    close(client_sock);
    sleep(5);
    // not crashed
    ASSERT_TRUE(event_base_get_num_events(evt_base, EV_READ | EV_WRITE) > 0);

    END();
}

/*********************************************************
 * # SIGPIPE error
 * - client connects, but server not listening
 * - terminate client
 * => "Shutdown error #(13): Transport endpoint is not connected"
 *********************************************************/
bool test_sigpipe_error() {
    BEGIN();
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    sleep(1);
    close(client_sock);
    sleep(5);
    // not crashed
    ASSERT_TRUE(event_base_get_num_events(evt_base, EV_READ | EV_WRITE) > 0);

    END();
}

/*********************************************************
 * # event_del failure after fd closed
 * - fire SIGUSR1 in the background every 0.1 seconds
 * - client sends "GET / HTTP/1.1\r\n\r\n", and terminates.
 * - server sends "\n\n", and terminates.
 * => "[warn] Epoll MOD(4) on fd 13 failed. Old events were 6; read change was 2 (del); write change was 0 (none); close change was 0 (none): Bad file descriptor"
 *********************************************************/
bool test_event_del_failure() {
    BEGIN();
    int pid = send_SIGUSR1_background(100);
    int client_sock = connect_TCP("localhost", PROXY_PORT);
    int server_sock = passive_TCP(SERVER_PORT);  // prepare server
    auto conn_list = get_all_connections();
    auto conn = conn_list[0];
    ASSERT_EQUAL(1, conn_list.size());  // only has one connection so far

    auto request = "GET / HTTP/1.1\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(1);
    ASSERT_EQUAL(false, conn->is_fast_mode());  // now client is in slow-mode
    close(client_sock);

    int sock = accept_connection(server_sock);
    auto response = "\n\n";
    write_with_assert(sock, response, strlen(response));
    sleep(1);
    close(sock);
    sleep(3);
    // not crashed
    ASSERT_TRUE(event_base_get_num_events(evt_base, EV_READ | EV_WRITE) > 0);

    if (kill(pid, SIGINT) < 0) ERROR_EXIT("Kill error");
    if (waitpid(pid, &stat, 0) < 0) ERROR_EXIT("Waitpid error");
    END();
}

int main() {
    Test tests[] = {
        test_close_with_rst_instead_of_fin,
        test_signal_not_handled_correctly,
        test_sigpipe_error,
        test_event_del_failure,
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
