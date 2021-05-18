#include "helper.h"


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
        sleep(3);  // wait for startup to complete
    }
}

void teardown() {
    close(parent_inbox[PIPE_R]);
    close(child_inbox[PIPE_W]);
    send_SIGINT_to(child_pid);
    if (waitpid(child_pid, &status, 0) < 0) ERROR_EXIT("Waitpid error");
}

// fork a process and return its pid
int send_SIGUSR1_background(int milisec) {
    if (int pid = fork(); pid < 0) {
        ERROR_EXIT("Cannot fork");
    } else if (pid == 0) {
        while (true) {
            send_SIGUSR1_to(child_pid);
            usleep(milisec * 1000);
        }
        exit(0);
    } else {
        return pid;
    }
}

void proxy_run(string command) {
    command += ";";
    write_with_assert(child_inbox[PIPE_W], command.c_str(), command.size());
    send_SIGUSR2_to(child_pid);  // trigger command
    sleep(1);
    char res = getchar_with_timeout(parent_inbox[PIPE_R], 5);
    ASSERT_EQUAL('O', res);
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
    sleep(1);
    auto request = "GET / HTTP/1.1\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(2);

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
    sleep(1);

    send_SIGUSR1_to(child_pid);
    close(client_sock);
    sleep(15);  // wait for callback to trigger
    proxy_run("ASSERT_proxy_alive");

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
    proxy_run("ASSERT_proxy_alive");

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
    int server_sock = passive_TCP(SERVER_PORT, true);  // prepare server
    sleep(1);
    proxy_run("save_a_connection");  // keep track this connection

    auto request = "GET / HTTP/1.1\r\n\r\n";
    write_with_assert(client_sock, request, strlen(request));
    sleep(2);
    proxy_run("ASSERT_not_fast_mode");  // now client is in slow-mode
    close(client_sock);

    int sock = accept_connection(server_sock);
    auto response = "\n\n";
    write_with_assert(sock, response, strlen(response));
    sleep(2);
    close(sock);
    sleep(5);
    proxy_run("ASSERT_proxy_alive");

    send_SIGINT_to(pid);
    if (waitpid(pid, &status, 0) < 0) ERROR_EXIT("Waitpid error");
    END();
}

int main() {
    Test tests[] = {
        test_close_with_rst_instead_of_fin,
        test_signal_not_handled_correctly,
        test_sigpipe_error,
        test_event_del_failure,
    };
    if (signal(SIGPIPE, [](int){abort();}) == SIG_ERR) {
        ERROR_EXIT("Cannot setup SIGPIPE handler");
    }
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
