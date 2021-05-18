#include "helper.h"
/*
 * ls_proxy.h should be adjusted first.
 */
#include "../src/llhttp/llhttp.h"
#define HIST_CACHE_SIZE     20   // less than total size of data
#define MAX_HYBRID_SIZE     HIST_CACHE_SIZE
#define SOCK_IO_BUF_SIZE    500  // more than total size of data; at least 380
#define MAX_CIRCULAR_SIZE   SOCK_IO_BUF_SIZE - 1  // max storable size


Filebuf* filebuf = NULL;
Hybridbuf* hybridbuf = NULL;
Circularbuf* circularbuf = NULL;
vector<Filebuf*> filebuf_tmp;  // to be recycled Filebufs
vector<string> data = {
    "hello",  /* 5 */
    "worldworld worldwor ldworldwo rldworldw orld",  /* 44 */
    "!?/\\)(><}{\"'@#$%^&*_-+!:;.`~|\r\n\t\n",       /* 33 */
    {'\0', '\1', '\xca', '\xfe', '\xba', '\xbe'},    /* 6 */
};


void setup() {
    filebuf = new Filebuf("filebuf");
    hybridbuf = new Hybridbuf("hybridbuf");
    circularbuf = new Circularbuf();
}

void teardown() {
    delete filebuf;
    delete hybridbuf;
    delete circularbuf;
    for (auto fb : filebuf_tmp) {
        delete fb;
    }
    filebuf_tmp.clear();
}

bool test_filebuf_nonblocking() {
    BEGIN();
    int fd = filebuf->get_fd();
    while (read(fd, global_buffer, sizeof(global_buffer)) > 0) {}
    END();
}

bool _rw_helper(Filebuf* buf) {
    // store data to buffer
    size_t count = 0;
    for (auto d : data) {
        buf->store(d.c_str(), d.size());
        count += d.size();
    }
    ASSERT_EQUAL(count, buf->data_size);
    buf->rewind();

    // fetch from buffer (REF: read_all() implementation)
    char* ptr = global_buffer;
    int remain = count;
    while (remain > 0) {
        count = buf->fetch(ptr, remain);
        if (count > 0) {
            ptr += count;  // move ptr
            remain -= count;
        } else {
            break;
        }
    }
    ASSERT_EQUAL(0, remain);
    string all_data;
    for (auto d : data) all_data += d;
    ASSERT_EQUAL(0, memcmp(all_data.c_str(), global_buffer, all_data.size()));
    return true;
}

bool test_filebuf_rw() {
    BEGIN();
    ASSERT_TRUE(_rw_helper(filebuf));
    END();
}

bool _clear_helper(Filebuf* buf) {
    // first time
    buf->store(data[0].c_str(), data[0].size());
    buf->clear();
    ASSERT_EQUAL(0, (int)buf->data_size);
    ASSERT_EQUAL(0, (int)buf->fetch(global_buffer, sizeof(global_buffer)));

    // second time
    buf->store(data[1].c_str(), data[1].size());
    buf->clear();
    ASSERT_EQUAL(0, (int)buf->data_size);
    ASSERT_EQUAL(0, (int)buf->fetch(global_buffer, sizeof(global_buffer)));
    return true;
}

bool test_filebuf_clear() {
    BEGIN();
    ASSERT_TRUE(_clear_helper(filebuf));
    END();
}

bool test_hybridbuf_rw() {
    BEGIN();
    // overflow to file when memory is full, but the result should be correct
    ASSERT_TRUE(_rw_helper(hybridbuf));
    END();
}

bool test_hybridbuf_clear() {
    BEGIN();
    ASSERT_TRUE(_clear_helper(hybridbuf));
    END();
}

int _create_empty_fd_helper() {
    Filebuf* fb = new Filebuf();
    filebuf_tmp.push_back(fb);
    return fb->get_fd();
}

// create a new Filebuf with data, and append it to filebuf_tmp
int _create_nonempty_fd_helper(size_t min_size = 1) {
    Filebuf* fb = new Filebuf();
    filebuf_tmp.push_back(fb);

    while (fb->data_size < min_size) {
        for (auto d : data) {
            fb->store(d.c_str(), d.size());
        }
    }
    ASSERT_GT(fb->data_size, min_size);
    fb->rewind();
    return fb->get_fd();
}

bool test_circularbuf_r() {
    BEGIN();
    int fd = _create_nonempty_fd_helper();
    auto fbuf = filebuf_tmp.back();
    auto stat = circularbuf->read_all_from(fd);

    // should reach file's eof
    ASSERT_TRUE(stat.has_eof);
    ASSERT_EQUAL(fbuf->data_size, stat.nbytes);
    // check for size/space functions
    auto space = circularbuf->remaining_space();
    auto size = circularbuf->data_size();
    ASSERT_EQUAL(stat.nbytes, size);
    ASSERT_EQUAL((size_t)MAX_CIRCULAR_SIZE, size + space);

    END();
}

bool test_circularbuf_overflow() {
    BEGIN();
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);
    auto stat = circularbuf->read_all_from(fd);

    // should not reach eof
    ASSERT_FALSE(stat.has_eof);
    // should not overflow
    ASSERT_LT(circularbuf->data_size(), (size_t)MAX_CIRCULAR_SIZE + 1);
    // check for size/space functions
    auto space = circularbuf->remaining_space();
    auto size = circularbuf->data_size();
    ASSERT_EQUAL(stat.nbytes, size);
    ASSERT_EQUAL((size_t)MAX_CIRCULAR_SIZE, size + space);

    END();
}

bool test_circularbuf_w() {
    BEGIN();
    // fill buffer
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);
    auto fbuf = filebuf_tmp.back();
    auto stat = circularbuf->read_all_from(fd);
    ASSERT_EQUAL(0, (int)circularbuf->remaining_space());  // should be full
    auto data_size = circularbuf->data_size();

    // write out
    int fd_out = _create_empty_fd_helper();
    auto fbuf_out = filebuf_tmp.back();
    stat = circularbuf->write_all_to(fd_out);
    ASSERT_EQUAL(0, (int)circularbuf->data_size());  // should be empty
    ASSERT_EQUAL(data_size, stat.nbytes);            // size should match

    // compare content
    fbuf->rewind();
    fbuf_out->rewind();
    ASSERT_TRUE(compare_fd_helper(fbuf->get_fd(), fbuf_out->get_fd()));

    END();
}

int main() {
    Test tests[] = {
        test_filebuf_nonblocking,
        test_filebuf_rw,
        test_filebuf_clear,
        test_hybridbuf_rw,
        test_hybridbuf_clear,
        test_circularbuf_r,
        test_circularbuf_overflow,
        test_circularbuf_w,
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
