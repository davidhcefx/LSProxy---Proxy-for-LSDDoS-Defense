#include "helper.h"


bool test_reply_503() {
    BEGIN("Testing reply_503...");
    const short port = 8099;
    int master_sock = passive_TCP(port);
    int sock = connect_TCP("localhost", port);
    auto count = reply_with_503_unavailable(sock);

    // check return size
    ASSERT_EQUAL(read_all(master_sock, global_buffer, count).nbytes, count);
    // compare content
    char local_buffer[sizeof(global_buffer)];
    int file_fd = open("utils/503.html", O_RDONLY);
    assert(file_fd > 0);
    read_all(file_fd, local_buffer, count);
    ASSERT_EQUAL(0, memcmp(global_buffer, local_buffer, count));
    END();
}

int main() {
    int failed = 0;
    if (!test_reply_503()) {
        printf("\n%18s^^ THIS TEST FAILED!! ^^%18s\n\n", DASH30, DASH30);
        failed++;
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