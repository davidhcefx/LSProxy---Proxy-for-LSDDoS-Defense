#include "ls_proxy.h"


Filebuf::Filebuf(): data_size{0} {
    char name[] = "/tmp/ls_proxy_buf_XXXXXX";
    if ((fd = mkstemp(name)) < 0) {
        ERROR_EXIT("Cannot mkstemp");
    }
    if (make_file_nonblocking(fd) < 0) {
        ERROR_EXIT("Cannot make file nonblocking");
    }
    file_name = string(name);
    LOG2("#%d = '%s'\n", fd, name);
}

void Filebuf::clear() {
    data_size = 0;
    // clear file
    if (ftruncate(fd, 0) < 0) {
        if (errno == EINTR && ftruncate(fd, 0) == 0) {  // if interrupted
            // pass
        } else {
            ERROR_EXIT("Cannot truncate file '%s'", file_name.c_str());
        }
    }
    rewind();
}

void Filebuf::_file_write(const char* data, size_t size) {
    auto remain = size;
    for (auto i = 0; i < 10 && remain > 0; i++) {  // try 10 times
        auto count = write(fd, data, remain);
        if (count > 0) {
            data += count;  // move ptr
            remain -= count;
        } else if (errno != EAGAIN && errno != EINTR) {  // not blocking nor interrupt
            ERROR("Write failed (retrying)");
            usleep(100);  // wait for 100us
        }
    }
    if (remain > 0) {
        WARNING("%lu bytes could not be written and were lost", remain);
    }
    data_size += size - remain;
    LOG2("Wrote %lu bytes to '%s'\n", size - remain, file_name.c_str());
}

/* ========================================================================= */

void Hybridbuf::store(const char* data, size_t size) {
    // first try to use memory
    auto count = _buf_write(data, size);
    size -= count;
    if (size > 0) {
        // then use file
        _file_write(data + count, size);
    }
}

ssize_t Hybridbuf::fetch(char* result, size_t max_size) {
    // first try to read some from memory
    auto count = _buf_read(result, max_size);
    if (count > 0) {
        return count;
    } else {
        // then from file
        return _file_read(result, max_size);
    }
}

size_t Hybridbuf::_buf_write(const char* data, size_t size) {
    auto space = _buf_remaining_space();
    if (space > 0) {
        size = min(size, space);  // restricted by space
        memcpy(buffer + next_pos, data, size);
        next_pos += size;
        data_size += size;
        LOG2("Buffer occupied %lu bytes out of %lu\n", size, space);
        return size;
    } else {
        return 0;
    }
}

size_t Hybridbuf::_buf_read(char* result, size_t max_size) {
    auto size = _buf_unread_data_size();
    if (size > 0) {
        size = min(size, max_size);  // restricted by max_size
        memcpy(result, buffer + next_pos, size);
        next_pos += size;
        LOG2("Buffer read %lu bytes\n", size);
        return size;
    } else {
        return 0;
    }
}

/* ========================================================================= */

size_t Circularbuf::copy_from(const char* data, size_t size) {
    size_t remain = min(size, remaining_space());  // restricted by space
    size_t count = 0;
    if (remain > 0) {
        if (end_ptr >= start_ptr) {
            // copy towards right-end
            size_t space_to_rightend = buffer + sizeof(buffer) - end_ptr;
            count = min(remain, space_to_rightend);
            memcpy(end_ptr, data, count);
            remain -= count;
            if (count < space_to_rightend) {
                end_ptr += count;
            } else {
                end_ptr = buffer;  // wrap to start
            }
        }
        if (remain > 0) {
            // copy towards start_ptr
            memcpy(end_ptr, data + count, remain);
            end_ptr += remain;
            count += remain;
        }
    }
    return count;
}

struct io_stat Circularbuf::read_all_from(int fd) {
    auto stat = read_all(fd, global_buffer, remaining_space());
    if (stat.has_error && !(errno == EAGAIN || errno == EINTR)) {
        ERROR("Read failed (#%d)", fd);
    }
    // copy to internal buffer
    assert(copy_from(global_buffer, stat.nbytes) == stat.nbytes);
    return stat;
}

struct io_stat Circularbuf::write_all_to(int fd) {
    struct io_stat stat;
    auto remain = data_size();
    auto orig_data_size = remain;
    if (remain > 0) {
        if (start_ptr > end_ptr) {
            // fetch towards right-end
            size_t size_to_rightend = buffer + sizeof(buffer) - start_ptr;
            stat = write_all(fd, start_ptr, size_to_rightend);
            remain -= stat.nbytes;
            if (stat.nbytes < size_to_rightend) {
                start_ptr += stat.nbytes;
                assert(stat.has_error);  // must be having some error
                return stat;
            } else {
                start_ptr = buffer;  // wrap to start
            }
        }
        if (remain > 0) {
            // fetch towards end_ptr
            stat = write_all(fd, start_ptr, remain);
            start_ptr += stat.nbytes;
        }
    }
    stat.nbytes = orig_data_size - data_size();
    return stat;
}
