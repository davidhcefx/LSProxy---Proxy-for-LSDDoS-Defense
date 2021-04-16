#include "../src/ls_proxy.h"
// override macros
#define HIST_CACHE_SIZE     17   // less than data total size
#define MAX_HYBRID_SIZE  HIST_CACHE_SIZE
#define SOCK_IO_BUF_SIZE    150  // more than twice data total size
#define MAX_CIRCULAR_SIZE SOCK_IO_BUF_SIZE
#define DASH30              "=============================="
#define BEGIN(msg)          printf(msg)
#define DONE()              printf("done.\n"); return true;
// #define REPLACE_RET_FALSE_WITH_ASSERT
#ifdef REPLACE_RET_FALSE_WITH_ASSERT
#define ASSERT_OR_RET(cond) assert(cond);
#else
#define ASSERT_OR_RET(cond) if (!(cond)) return false;
#endif


char global_buffer[1024];
Filebuf* filebuf = NULL;
Hybridbuf* hybridbuf = NULL;
Circularbuf* circularbuf = NULL;
vector<Filebuf*> filebuf_pool;  // for temporary usage
vector<string> data = {
    "hello",
    "worldworldworldworldworldworldworldworld",
    "_-!",
    "\n \x01\x00\xde\xad\xbe\xef\x80\t\r\n."
};

void setup() {
    filebuf = new Filebuf();
    hybridbuf = new Hybridbuf();
    circularbuf = new Circularbuf();
}

void teardown() {
    delete filebuf;
    delete hybridbuf;
    delete circularbuf;
    for (auto fb : filebuf_pool) {
        delete fb;
    }
}

bool test_file_nonblocking() {
    BEGIN("Testing file nonblocking...");
    int fd = filebuf->get_fd();
    while (read(fd, global_buffer, sizeof(global_buffer)) > 0) {}
    DONE();
}

bool _rw_helper(Filebuf* buf) {
    // store to buffer
    int count = 0;
    for (auto d : data) {
        buf->store(d.c_str(), d.size());
        count += d.size();
    }
    ASSERT_OR_RET(buf->data_size == count);
    buf->rewind();

    // fetch from buffer (REF: read_all implementation)
    char* ptr = global_buffer;
    int remain = count;
    while (remain > 0) {
        count = buf->fetch(buf, remain);
        if (count > 0) {
            ptr += count;  // move ptr
            remain -= count;
        } else {
            break;
        }
    }
    ASSERT_OR_RET(remain == 0);
    string all_data;
    for (auto d : data) all_data += d;
    ASSERT_OR_RET(memcmp(all_data.c_str(), global_buffer, all_data.size()) == 0);
    return true;
}

bool test_file_rw() {
    BEGIN("Testing file r/w...");
    ASSERT_OR_RET(_rw_helper(filebuf));
    DONE();
}

bool _clear_helper(Filebuf* buf) {
    // first time
    buf->store(data[0].c_str(), data[0].size());
    buf->clear();
    int count = buf->fetch(global_buffer, sizeof(global_buffer));
    ASSERT_OR_RET(count == -1 && errno == EAGAIN);
    // second time
    buf->store(data[1].c_str(), data[1].size());
    buf->clear();
    count = buf->fetch(global_buffer, sizeof(global_buffer));
    ASSERT_OR_RET(count == -1 && errno == EAGAIN);
    return true;
}

bool test_file_clear() {
    BEGIN("Testing file clear...");
    ASSERT_OR_RET(_clear_helper(filebuf));
    DONE();
}

bool test_hybrid_rw() {
    BEGIN("Testing hybridbuf r/w...");
    // overflow when memory is full, but the result should be correct
    ASSERT_OR_RET(_rw_helper(hybridbuf));
    DONE();   
}

bool test_hybrid_clear() {
    BEGIN("Testing hybridfile clear...");
    ASSERT_OR_RET(_clear_helper(hybridbuf));
    DONE();
}

int _create_empty_fd_helper() {
    Filebuf* fb = new Filebuf();
    filebuf_pool.push_back(fb);
    return fb->get_fd();
}

// create a new Filebuf with data, and append it to filebuf_pool
int _create_nonempty_fd_helper(int min_size = 1) {
    Filebuf* fb = new Filebuf();
    filebuf_pool.push_back(fb);
    while (fb->data_size < min_size) {
        for (auto d : data) {
            fb->store(d.c_str(), d.size());
        }
    }
    ASSERT_OR_RET(fb->data_size > min_size);
    fb->rewind();
    return fb->get_fd();
}

bool test_circular_r() {
    BEGIN("Testing circularbuf read...");
    int fd = _create_nonempty_fd_helper();
    auto fbuf = filebuf_pool.back();
    auto stat = circularbuf->read_all_from(fd);
    // should reach eof
    ASSERT_OR_RET(stat.has_eof == true);
    ASSERT_OR_RET(stat.nbytes == fbuf->data_size);
    // check for size/space functions
    int space = circularbuf->remaining_space();
    int size = circularbuf->data_size();
    ASSERT_OR_RET(stat.nbytes == size);
    ASSERT_OR_RET(size + space == MAX_CIRCULAR_SIZE);
    DONE();
}

bool test_circular_overflow() {
    BEGIN("Testing circularbuf overflow...");
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);    
    auto stat = circularbuf->read_all_from(fd);
    // should not reach eof
    ASSERT_OR_RET(stat.has_eof == false);
    // should not overflow (last byte don't store data)
    ASSERT_OR_RET(circularbuf->data_size() < MAX_CIRCULAR_SIZE);
    // check for size/space functions
    int space = circularbuf->remaining_space();
    int size = circularbuf->data_size();
    ASSERT_OR_RET(stat.nbytes == size);
    ASSERT_OR_RET(size + space == MAX_CIRCULAR_SIZE);
    DONE();
}

// compare if the data within the two fd are the same
bool _compare_fd_helper(int fd1, int fd2) {
    char local_buffer [sizeof(global_buffer)];
    int n1 = read_all(fd1, global_buffer, sizeof(global_buffer)).nbytes;
    int n2 = read_all(fd2, local_buffer, sizeof(local_buffer)).nbytes;
    ASSERT_OR_RET(n1 == n2);
    ASSERT_OR_RET(memcmp(global_buffer, local_buffer, n1) == 0);
    return true;
}

bool test_circular_w() {
    BEGIN("Testing circularbuf write...", );
    // fill buffer
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);
    auto fbuf = filebuf_pool.back();
    auto stat = circularbuf->read_all_from(fd);
    // write out
    int fd_out = _create_empty_fd_helper();
    auto fbuf_out = filebuf_pool.back();
    stat = circularbuf->write_all_to(fd_out);
    // buffer should be empty
    ASSERT_OR_RET(circularbuf->data_size == 0);

    // compare
    fbuf->rewind();
    fbuf_out->rewind();
    ASSERT_OR_RET(_compare_fd_helper(fbuf->get_fd(), fbuf_out->get_fd()));
    DONE();
}

int main() {
    typedef bool (*Test)();
    Test tests[] = {
        test_file_nonblocking,
        test_file_rw,
        test_file_clear,
        test_hybrid_rw,
        test_hybrid_clear,
        test_circular_r,
        test_circular_overflow,
        test_circular_w,
    };
    int failed = 0;
    for (auto t in tests) {
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
