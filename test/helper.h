#ifndef TEST_HELPER_H_
#define TEST_HELPER_H_
#include "ls_proxy.h"
#include <vector>
#define DASH30      "=============================="
/* put these at the start and the end of every tests */
#define BEGIN(msg)  printf(msg)
#define END()       printf("done.\n"); return true;
#define USE_RET_FALSE_AS_ASSERT
#ifdef  USE_RET_FALSE_AS_ASSERT
#define ASSERT_TRUE(cond)   if (!(cond)) return false;
#else
#define ASSERT_TRUE(cond)   assert(cond);
#endif  // USE_RET_FALSE_AS_ASSERT
#define ASSERT_EQUAL(expect, actual)  ASSERT_TRUE(expect == actual)
using std::vector;


// function pointer of tests
typedef bool (*Test)();

// compare if the data within the two fd are the same
bool compare_fd_helper(int fd1, int fd2) {
    char local_buffer [sizeof(global_buffer)];
    int n1 = read_all(fd1, global_buffer, sizeof(global_buffer)).nbytes;
    int n2 = read_all(fd2, local_buffer, sizeof(local_buffer)).nbytes;
    ASSERT_EQUAL(n1, n2);
    ASSERT_EQUAL(0, memcmp(global_buffer, local_buffer, n1));
    return true;
}

#endif  // TEST_HELPER_H_
