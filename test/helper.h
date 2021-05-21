#ifndef TEST_HELPER_H_
#define TEST_HELPER_H_
#include <sys/wait.h>
#include <iostream>
#include <sstream>
#include "ls_proxy.h"
using std::cin;
using std::cout;
using std::endl;
using std::stringstream;
#define SERVER_PORT  8787
#define PROXY_PORT   8099
#define DASH30      "=============================="
#define DASH10      "=========="
#define PIPE_R      0
#define PIPE_W      1
/* put the following at the start and the end of each test */
#define BEGIN()     LOG1("\n[" DASH10 " Running %s " DASH10 "]\n", __func__);
#define END()       LOG1("----------Done----------\n"); return true;
// #define RETURN_FALSE_AS_ASSERT
#ifdef  RETURN_FALSE_AS_ASSERT
#define ASSERT(op, var1, var2)  { if (!((var1) op (var2))) return false; }
#else
#define ASSERT(op, var1, var2)  \
    { auto v1 = var1, v2 = var2; \
      if (!((v1) op (v2))) { \
        cout << v1 << endl << v2 << endl; \
        teardown(); \
        assert((v1) op (v2)); \
      } }
#endif  // RETURN_FALSE_AS_ASSERT
#define ASSERT_EQUAL(expect, actual)       ASSERT(==, expect, actual)
#define ASSERT_NOT_EQUAL(nexpect, actual)  ASSERT(!=, nexpect, actual)
#define ASSERT_GT(actual, ref)             ASSERT(>, actual, ref)
#define ASSERT_LT(actual, ref)             ASSERT(<, actual, ref)
#define ASSERT_TRUE(actual)                ASSERT_EQUAL(true, actual)
#define ASSERT_FALSE(actual)               ASSERT_EQUAL(false, actual)


// pipes for forked proxy
extern int parent_inbox[2];
extern int child_inbox[2];

// type of tests
typedef bool (*Test)();


void setup();
void teardown();

inline void display_this_test_failed_msg() {
    printf("\n%18.18s^^ THIS TEST FAILED!! ^^%18.18s\n\n", DASH30, DASH30);
}

inline void display_tests_summary(int fails) {
    printf(DASH30 DASH30 "\n");

    if (fails > 0) {
        printf("%23s%d TEST FAILED!\n", "", fails);
    } else {
        printf("%24sALL TEST PASSED.\n", "");
    }
    printf(DASH30 DASH30 "\n\n");
}

// assert written size matched
inline void write_with_assert(int fd, const char* buffer, size_t size) {
    auto stat = write_all(fd, buffer, size);
    assert(size == stat.nbytes);
}

// fd should be nonblocking
inline char getchar_with_timeout(int fd, int seconds) {
    char buf[1] = {'_'};
    if (read(fd, buf, 1) < 0) {
        sleep(seconds);
        (void)(read(fd, buf, 1) + 1);
    }
    return buf[0];
}

inline void read_until(char delim, int fd, char* buf, size_t max_size) {
    while (true) {
        if (auto count = read(fd, buf, max_size); count > 0) {
            buf[count] = '\0';
            if (strchr(buf, delim) != NULL) break;
            buf += count;  // move ptr
            max_size -= count;
        } else if (count < 0 && errno == EAGAIN) {
            // do nothing
        } else {
            break;
        }
    }
}

// return accepted socket fd
inline int accept_connection(int master_sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(master_sock, (struct sockaddr*)&addr, &addr_len);
    if (evutil_make_socket_nonblocking(sock) < 0) {
        ERROR_EXIT("Cannot make socket nonblocking");
    }
    return sock;
}

// compare if the data are identical, for min(len(fd1), len(fd2)) bytes
inline bool compare_fd_helper(int fd1, int fd2) {
    char local_buffer[sizeof(global_buffer)];
    int n1 = read_all(fd1, global_buffer, sizeof(global_buffer)).nbytes;
    int n2 = read_all(fd2, local_buffer, sizeof(local_buffer)).nbytes;
    ASSERT_EQUAL(0, memcmp(global_buffer, local_buffer, min(n1, n2)));
    return true;
}

inline int make_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// check if msg has a "Content-Length:" header and its value is correct
inline bool check_content_length_matched(const string& msg) {
    const char* header = "\r\nContent-Length:";
    const char* crlfcrlf = "\r\n\r\n";

    size_t idx = msg.find(header);
    ASSERT_NOT_EQUAL(string::npos, idx);
    idx += strlen(header);
    size_t length = atoi(msg.c_str() + idx);

    idx = msg.find(crlfcrlf, idx);
    ASSERT_NOT_EQUAL(string::npos, idx);
    idx += strlen(crlfcrlf);
    ASSERT_EQUAL(length, msg.size() - idx);
    return true;
}

inline void send_SIGINT_to(int pid) {
    if (kill(pid, SIGINT) < 0) ERROR_EXIT("Send SIGINT error");
}

inline void send_SIGUSR1_to(int pid) {
    if (kill(pid, SIGUSR1) < 0) ERROR_EXIT("Send SIGUSR1 error");
}

inline void send_SIGUSR2_to(int pid) {
    if (kill(pid, SIGUSR2) < 0) ERROR_EXIT("Send SIGUSR2 error");
}

// run proxy and block execution
void run_proxy(unsigned short port, const char* server_addr, \
               unsigned short server_port);

bool execute_command(string command);

// return firstly encountered Connection ptr or NULL
inline Connection* get_first_connection() {
    vector<struct event*> evt_list;
    event_base_foreach_event(evt_base, add_to_list, &evt_list);
    for (auto evt : evt_list) {
        if (auto conn = get_associated_conn(evt)) return conn;
    }
    return NULL;
}

inline uint8_t method_to_uint8(const string& method) {
    if (method == "HEAD") return HTTP_HEAD;
    if (method == "GET") return HTTP_GET;
    if (method == "POST") return HTTP_POST;
    if (method == "PUT") return HTTP_PUT;
    return 0;
}

#endif  // TEST_HELPER_H_
