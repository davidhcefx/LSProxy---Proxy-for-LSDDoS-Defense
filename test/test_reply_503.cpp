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

bool test_reply_503_unit_test() {
    BEGIN();
    int master_sock = passive_TCP(SERVER_PORT, true);
    int client_sock = connect_TCP("localhost", SERVER_PORT);
    int sock = accept_connection(master_sock);
    auto count = reply_with_503_unavailable(sock);

    // return size should be correct
    ASSERT_EQUAL(read_all(client_sock, global_buffer, count).nbytes, count);
    // check content length
    global_buffer[count] = '\0';
    string msg(global_buffer);
    ASSERT_TRUE(check_content_length_matched(msg));

    close(client_sock);
    close(sock);
    close(master_sock);
    END();
}

/******************************************************************************
 * - create MAX_CONNECTION number of connections
 * - a client connects, and it should get the 503 message
 ******************************************************************************/
bool test_reply_503_when_reached_MAX_CONNECTION() {
    vector<int> clients;
    for (int i = 0; i < MAX_CONNECTION; i++) {
        clients.push_back(connect_TCP("localhost", PROXY_PORT));
    }
    int client_sock = connect_TCP("localhost", PROXY_PORT);

    auto stat = read_all(client_sock, global_buffer, sizeof(global_buffer));
    ASSERT_GT((int)stat.nbytes, 0);
    // check content length
    global_buffer[stat.nbytes] = '\0';
    string msg(global_buffer);
    ASSERT_TRUE(check_content_length_matched(msg));

    close(client_sock);
    for (auto fd : clients) close(fd);
    END();
}

int main() {
    Test tests[] = {
        test_reply_503_unit_test,
        test_reply_503_when_reached_MAX_CONNECTION
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
