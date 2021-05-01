#include "helper.h"
/*
 * ls_proxy.h should be adjusted first.
 */
#define HIST_CACHE_SIZE     17   // less than total size of data
#define MAX_HYBRID_SIZE     HIST_CACHE_SIZE
#define SOCK_IO_BUF_SIZE    150  // more than twice total size of data
#define MAX_CIRCULAR_SIZE   SOCK_IO_BUF_SIZE


Filebuf* filebuf = NULL;
Hybridbuf* hybridbuf = NULL;
Circularbuf* circularbuf = NULL;
vector<Filebuf*> filebuf_tmp;  // to be recycled Filebufs
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
    for (auto fb : filebuf_tmp) {
        delete fb;
    }
}

bool test_file_nonblocking() {
    BEGIN("Testing file nonblocking...");
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

bool test_file_rw() {
    BEGIN("Testing file r/w...");
    ASSERT_TRUE(_rw_helper(filebuf));
    END();
}

bool _clear_helper(Filebuf* buf) {
    // first time
    buf->store(data[0].c_str(), data[0].size());
    buf->clear();
    ASSERT_EQUAL(0, buf->data_size);
    ASSERT_EQUAL(0, buf->fetch(global_buffer, sizeof(global_buffer)));

    // second time
    buf->store(data[1].c_str(), data[1].size());
    buf->clear();
    ASSERT_EQUAL(0, buf->data_size);
    ASSERT_EQUAL(0, buf->fetch(global_buffer, sizeof(global_buffer)));
    return true;
}

bool test_file_clear() {
    BEGIN("Testing file clear...");
    ASSERT_TRUE(_clear_helper(filebuf));
    END();
}

bool test_hybrid_rw() {
    BEGIN("Testing hybridbuf r/w...");
    // overflow to file when memory is full, but the result should be correct
    ASSERT_TRUE(_rw_helper(hybridbuf));
    END();   
}

bool test_hybrid_clear() {
    BEGIN("Testing hybridfile clear...");
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
    ASSERT_TRUE(fb->data_size > min_size);
    fb->rewind();
    return fb->get_fd();
}

bool test_circular_r() {
    BEGIN("Testing circularbuf read...");
    int fd = _create_nonempty_fd_helper();
    auto fbuf = filebuf_tmp.back();
    auto stat = circularbuf->read_all_from(fd);

    // should reach file's eof
    ASSERT_EQUAL(true, stat.has_eof);
    ASSERT_EQUAL(fbuf->data_size, stat.nbytes);
    // check for size/space functions
    auto space = circularbuf->remaining_space();
    auto size = circularbuf->data_size();
    ASSERT_EQUAL(stat.nbytes, size);
    ASSERT_EQUAL(MAX_CIRCULAR_SIZE, size + space);

    END();
}

bool test_circular_overflow() {
    BEGIN("Testing circularbuf overflow...");
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);    
    auto stat = circularbuf->read_all_from(fd);

    // should not reach eof
    ASSERT_EQUAL(false, stat.has_eof);
    // should not overflow (last byte don't store data)
    ASSERT_TRUE(circularbuf->data_size() < MAX_CIRCULAR_SIZE);
    // check for size/space functions
    auto space = circularbuf->remaining_space();
    auto size = circularbuf->data_size();
    ASSERT_EQUAL(stat.nbytes, size);
    ASSERT_EQUAL(MAX_CIRCULAR_SIZE, size + space);

    END();
}

bool test_circular_w() {
    BEGIN("Testing circularbuf write...");
    // fill buffer
    int fd = _create_nonempty_fd_helper(MAX_CIRCULAR_SIZE);
    auto fbuf = filebuf_tmp.back();
    auto stat = circularbuf->read_all_from(fd);
    // write out
    int fd_out = _create_empty_fd_helper();
    auto fbuf_out = filebuf_tmp.back();
    stat = circularbuf->write_all_to(fd_out);

    // buffer should be empty
    ASSERT_EQUAL(0, circularbuf->data_size());
    // compare
    fbuf->rewind();
    fbuf_out->rewind();
    ASSERT_TRUE(compare_fd_helper(fbuf->get_fd(), fbuf_out->get_fd()));

    END();
}

int main() {
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
    for (auto t : tests) {
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
