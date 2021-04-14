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

void Filebuf::_file_write(const char* data, int size) {
    int remain = size;
    for (int i = 0; i < 10 && remain > 0; i++) {  // try 10 times
        int count = write(fd, data, remain);
        if (count > 0) {
            remain -= count;
        } else if (errno != EAGAIN && errno != EINTR) {  // not blocking nor interrupt
            ERROR("Write failed");
            usleep(100);  // wait for 100us
        }
    }
    if (remain > 0) {
        WARNING("%d bytes could not be written and were lost", remain);
    }
    data_size += size - remain;
    LOG2("Wrote %d bytes to '%s'\n", size - remain, file_name.c_str());
}

inline int Filebuf::_file_read(char* result, int max_size) {
    int count = read(fd, result, max_size);
    if (count < 0) {
        ERROR("Read failed '%s'", file_name.c_str());
    }
    return count;
}

inline void Filebuf::_file_rewind() {
    if (lseek(fd, 0, SEEK_SET) < 0) {
        ERROR_EXIT("Cannot lseek");
    }
}

/* ========================================================================= */

inline void Hybridbuf::store(const char* data, int size) {
    // first try to use memory
    int count = _buf_write(data, size);
    size -= count;
    if (size > 0) {
        // then use file
        _file_write(data + count, size);
    }
}

inline int Hybridbuf::fetch(char* result, int max_size) {
    // first try to read some from memory
    int count = _buf_read(result, max_size);
    if (count > 0) {
        return count;
    } else {
        // then from file
        return _file_read(result, max_size);
    }
}

inline void Hybridbuf::rewind() {
    _file_rewind();
    _buf_rewind();
}

inline int Hybridbuf::_buf_remaining_space() {
    if (next_pos < MAX_HEADER_SIZE) {
        return MAX_HEADER_SIZE - next_pos;
    } else {
        return 0;
    }
}

inline int Hybridbuf::_buf_unread_data_size() {
    return min(data_size, MAX_HEADER_SIZE) - next_pos;
}

int Hybridbuf::_buf_write(const char* data, int size) {
    int space = _buf_remaining_space();
    if (space > 0) {
        size = min(size, space);  // restricted by space
        memcpy(buffer + next_pos, data, size);
        next_pos += size;
        data_size += size;
        LOG2("Buffer occupied %d bytes out of %d\n", size, space);
        return size;
    } else {
        return 0;
    }
}

int Hybridbuf::_buf_read(char* result, int max_size) {
    int size = _buf_unread_data_size();
    if (size > 0) {
        size = min(size, max_size);  // restricted by max_size
        memcpy(result, buffer + next_pos, size);
        next_pos += size;
        LOG2("Buffer read %d bytes\n", size);
        return size;
    } else {
        return 0;
    }
}

/* ========================================================================= */

int Circularbuf::read_all(int fd) {

}

int Circularbuf::write_all(int fd) {

}

inline int Circularbuf::data_size() {
    if (end_ptr >= start_ptr) {
        return end_ptr - start_ptr;
    } else {
        sizeof(buffer) - (start_ptr - end_ptr);
    }
}

inline int Circularbuf::remaining_space() {
    return sizeof(buffer) - 1 - data_size();
}