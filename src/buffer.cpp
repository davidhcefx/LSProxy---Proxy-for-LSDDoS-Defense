#include "buffer.h"


Filebuf::Filebuf(const char* alias): data_size{0} {
    char name[] = TEMP_FOLDER "/ls_proxy_buf_XXXXXX";
    if ((fd = mkstemp(name)) < 0) { [[unlikely]]
        ERROR_EXIT("Cannot mkstemp");
    }
    file_name = string(name);
    LOG2("#%d = '%s' (%s)\n", fd, name, alias);
}

void Filebuf::clear() {
    data_size = 0;
    // clear file
    if (ftruncate(fd, 0) < 0) { [[unlikely]]
        if (errno == EINTR && ftruncate(fd, 0) == 0) {  // retry
            /* pass */
        } else {
            ERROR_EXIT("Cannot truncate '%s'", file_name.c_str());
        }
    }
    LOG3("File #%d: Cleared.\n", fd);
    rewind();
}

void Filebuf::copy_data(Filebuf* source, Filebuf* dest) {
    dest->clear();  // clear before storing
    source->rewind();
    ssize_t n;
    while ((n = source->fetch(global_buffer, sizeof(global_buffer))) > 0) {
        dest->store(global_buffer, n);
    }
}

void Filebuf::_file_write(const char* data, size_t size) {
    auto remain = size;
    for (int i = 0; i < 10 && remain > 0; i++) {  // try 10 times
        if (auto count = write(fd, data, remain); count > 0) {
            data += count;  // move ptr
            remain -= count;
        } else if (errno != EAGAIN && errno != EINTR) { [[unlikely]]
            /* file normally won't block nor interrupt, but still rule them out */
            ERROR_EXIT("Write failed");
        }
    }
    if (remain > 0) { [[unlikely]]
        WARNING("%lu bytes could not be written and were lost", remain);
    }
    data_size += size - remain;
    LOG3("File #%d: Wrote %lu bytes.\n", fd, size - remain);
}

/* ========================================================================= */

void Hybridbuf::store(const char* data, size_t size) {
    // try with memory, then file
    auto count = _buf_write(data, size);
    size -= count;
    if (size > 0) {
        _file_write(data + count, size);
    }
}

ssize_t Hybridbuf::fetch(char* result, size_t max_size) {
    // try read some from memory, then file
    if (auto count = _buf_read(result, max_size); count > 0) {
        return count;
    } else {
        return _file_read(result, max_size);
    }
}

void Hybridbuf::rewind(size_t amount) {
    if (amount == 0) {
        _file_rewind(0);
        _buf_rewind(0);
    } else {
        /* if amount is too big, it would cause _file_rewind to fail */
        size_t offset = data_size - amount;
        if (offset < sizeof(buffer)) {  // within buffer
            _file_rewind(0);
            _buf_rewind(next_pos - offset);
        } else {
            _file_rewind(amount);
        }
    }
}

size_t Hybridbuf::_buf_write(const char* data, size_t size) {
    if (auto space = _buf_remaining_space(); space > 0) {
        size = min(size, space);  // restricted by space
        memcpy(buffer + next_pos, data, size);
        next_pos += size;
        data_size += size;
        LOG3("Mem-buf #%d: Occupied %lu / %lu bytes.\n", fd, size, space);
        return size;
    } else {
        return 0;
    }
}

size_t Hybridbuf::_buf_read(char* result, size_t max_size) {
    if (auto size = _buf_unread_data_size(); size > 0) {
        size = min(size, max_size);  // restricted by max_size
        memcpy(result, buffer + next_pos, size);
        next_pos += size;
        LOG3("Mem-buf #%d: Read %lu bytes\n", fd, size);
        return size;
    } else {
        return 0;
    }
}

/* ========================================================================= */

void FIFOfilebuf::_file_rewind(size_t amt) {
    int whence = (amt == 0) ? SEEK_SET : SEEK_CUR;
    if (lseek(reader_fd, -amt, whence) < 0) {
        ERROR_EXIT("Cannot lseek");
    }
    if (lseek(fd, 0, whence) < 0) {  // writer fd
        ERROR_EXIT("Cannot lseek");
    }
    LOG3("File #%d #%d: Rewinded r:-%lu %s\n", fd, reader_fd, amt, \
         (amt == 0) ? "w:0 to head" : "");
}

/* ========================================================================= */

size_t Circularbuf::copy_from(const char* data, size_t size) {
    size_t count = 0;
    if ((size = min(size, remaining_space())) > 0) {  // restricted by space
        if (end_ptr >= start_ptr) {
            // copy towards right-end
            size_t space_to_rightend = buffer + sizeof(buffer) - end_ptr;
            count = min(size, space_to_rightend);
            memcpy(end_ptr, data, count);
            size -= count;
            if (count < space_to_rightend) {
                end_ptr += count;  // done
            } else {
                end_ptr = buffer;  // wrap to start
                if (size > 0) {
                    // copy towards start_ptr
                    memcpy(end_ptr, data + count, size);
                    end_ptr += size;
                    count += size;
                }
            }
        } else {
            // copy towards start_ptr
            memcpy(end_ptr, data, size);
            end_ptr += size;
            count += size;
        }
    }
    return count;
}

struct io_stat Circularbuf::read_all_from(int fd) {
    auto stat = read_all(fd, global_buffer, remaining_space());
    if (stat.has_error && errno != EAGAIN && errno != EINTR) { [[unlikely]]
        ERROR("Read failed (#%d)", fd);
    }
    // copy to internal buffer
    assert(copy_from(global_buffer, stat.nbytes) == stat.nbytes);
    return stat;
}

struct io_stat Circularbuf::write_all_to(int fd) {
    struct io_stat stat;
    size_t size, size_old;
    if ((size = size_old = data_size()) > 0) {
        if (start_ptr > end_ptr) {
            // fetch towards right-end
            size_t size_to_rightend = buffer + sizeof(buffer) - start_ptr;
            stat = write_all(fd, start_ptr, size_to_rightend);
            size -= stat.nbytes;
            if (stat.nbytes < size_to_rightend) { [[unlikely]]
                assert(stat.has_error);    // must be having some error
                start_ptr += stat.nbytes;  // done
            } else {
                start_ptr = buffer;  // wrap to start
                if (size > 0) {
                    // fetch towards end_ptr
                    stat = write_all(fd, start_ptr, size);
                    start_ptr += stat.nbytes;
                }
            }
        } else {
            // fetch towards end_ptr
            stat = write_all(fd, start_ptr, size);
            start_ptr += stat.nbytes;
        }
    }
    stat.nbytes = size_old - data_size();
    return stat;
}

void Circularbuf::dump_to(Filebuf* filebuf) {
    auto size = data_size();
    LOG2("Dumping %lu bytes from Circularbuf...\n", size);
    if (size > 0) {
        if (start_ptr > end_ptr) {
            // fetch towards right-end
            size_t size_to_rightend = buffer + sizeof(buffer) - start_ptr;
            filebuf->store(start_ptr, size_to_rightend);
            size -= size_to_rightend;
            start_ptr = buffer;  // wrap to start
            if (size > 0) {
                // fetch towards end_ptr
                filebuf->store(start_ptr, size);
                start_ptr += size;
            }
        } else {
            // fetch towards end_ptr
            filebuf->store(start_ptr, size);
            start_ptr += size;
        }
    }
}
