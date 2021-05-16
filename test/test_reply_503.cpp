#include "helper.h"
#define PORT  8099


bool test_reply_503() {
    BEGIN();
    int master_sock = passive_TCP(PORT);
    int client_sock = connect_TCP("localhost", PORT);
    int sock = accept_connection(master_sock);
    auto count = reply_with_503_unavailable(sock);

    // return size should be correct
    ASSERT_EQUAL(read_all(client_sock, global_buffer, count).nbytes, count);
    // check content length
    global_buffer[count] = '\0';
    string msg(global_buffer);
    ASSERT_TRUE(check_content_length_matched(msg));

    END();
}

int main() {
    if (signal(SIGPIPE, [](int){abort();}) == SIG_ERR) {
        ERROR_EXIT("Cannot setup SIGPIPE handler");
    }
    Test tests[] = {
        test_reply_503,
    };
    int failed = 0;
    for (auto t : tests) {
        if (!t()) {
            failed++;
            display_this_test_failed_msg();
        }
    }
    display_tests_summary(failed);

    return 0;
}
