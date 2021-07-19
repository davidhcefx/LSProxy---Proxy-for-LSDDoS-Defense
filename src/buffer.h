#ifndef SRC_BUFFER_H_
#define SRC_BUFFER_H_
#include "ls_proxy.h"


/**********************************************************
 * Class for using a file as buffer.                      *
 **********************************************************/
class Filebuf {
 public:
    size_t data_size;  // might cause undefined behavior when exceed size_t

    explicit Filebuf(const char* alias = "");
    virtual ~Filebuf() { close(fd); unlink(file_name.c_str()); }
    // make sure content was cleared before storing; data might lost upon disk failure
    virtual void store(const char* data, size_t size) {_file_write(data, size);}
    // error msg would be printed if failed
    virtual ssize_t fetch(char* res, size_t max_size) {return _file_read(res, max_size);}
    // rewind content cursor (for further reads); 0 is to the beginning
    virtual void rewind(size_t amount = 0) {_file_rewind(amount);}
    // clear content and rewind
    void clear();
    int get_fd() {return fd;}
    // copy data from source to dest (overwrite dest)
    static void copy_data(Filebuf* source, Filebuf* dest);

 protected:
    int fd;
    string file_name;

    // retry 10 times before data loss
    void _file_write(const char* data, size_t size);
    virtual ssize_t _file_read(char* res, size_t max_size) {
        return _read_and_log(fd, res, max_size);
    }
    // rewind backwards, amt=0 is to the beginning
    virtual void _file_rewind(size_t amt) {
        int whence = (amt == 0) ? SEEK_SET : SEEK_CUR;
        if (lseek(fd, -amt, whence) < 0) { ERROR_EXIT("Cannot lseek"); }
        LOG3("File #%d: Rewinded -%lu %s\n", fd, amt, (amt == 0) ? "to head" : "");
    }
    // error msg would be printed if failed
    static ssize_t _read_and_log(int fd, char* result, size_t max_size) {
        auto count = read(fd, result, max_size);
        if (count < 0) { [[unlikely]]
            ERROR("Read failed (#%d)", fd);
        }
        LOG3("File #%d: Read %lu bytes.\n", fd, count);
        return count;
    }
};


/**********************************************************
 * Class for using both memory and file as buffer.        *
 **********************************************************/
class Hybridbuf: public Filebuf {
 public:
    explicit Hybridbuf(const char* alias = ""): Filebuf{alias}, next_pos{0} {};
    void store(const char* data, size_t size) override;
    ssize_t fetch(char* result, size_t max_size) override;
    void rewind(size_t amount = 0) override;

 protected:
    char buffer[HIST_CACHE_SIZE];
    unsigned next_pos;  // next read/write position, range [0, HIST_CACHE_SIZE]

    size_t _buf_remaining_space() {
        return next_pos < sizeof(buffer) ? sizeof(buffer) - next_pos : 0;
    }
    size_t _buf_unread_data_size() {
        return min(data_size, sizeof(buffer)) - next_pos;
    }
    size_t _buf_write(const char* data, size_t size);
    size_t _buf_read(char* result, size_t max_size);
    // rewind backwards, amt=0 is to the beginning
    void _buf_rewind(unsigned amt) {
        LOG3("Mem-buf #%d: Rewinded -%u\n", fd, (amt == 0) ? next_pos : amt);
        next_pos -= (amt == 0) ? next_pos : amt;
    }
};


/**********************************************************
 * Class for file-based large-capacity FIFO buffer.       *
 **********************************************************/
class FIFOfilebuf: public Filebuf {
 public:
    explicit FIFOfilebuf(const char* alias = ""): Filebuf{alias} {
        reader_fd = open(file_name.c_str(), O_RDONLY);  // second fd
        if (reader_fd < 0) { [[unlikely]]
            ERROR_EXIT("Cannot open '%s'", file_name.c_str());
        }
        LOG2("#%d = '%s' (reader)\n", reader_fd, file_name.c_str());
    }
    int get_reader_fd() { return reader_fd; }
    /*
     * void store(const char* data, size_t size);
     * - Store to writer fd; make sure content was cleared before storing.
     * - Data might be lost upon disk failure.
     *
     * ssize_t fetch(char* result, size_t max_size);
     * - Fetch from reader fd; return 0 if there are no data available.
     * - Error message would be printed if failed.
     *
     * void rewind(size_t amount = 0);
     * - If amount == 0, rewind both reader & writer fd to the beginning.
     * - Otherwise, only reader fd would be rewinded.
     */
 private:
    int reader_fd;

    ssize_t _file_read(char* result, size_t max_size) override {
        return _read_and_log(reader_fd, result, max_size);
    }
    void _file_rewind(size_t amt) override;
};


/**********************************************************
 * Efficient in-memory circular buffer.                   *
 **********************************************************/
class Circularbuf {
 public:
    Circularbuf(): start_ptr{buffer}, end_ptr{buffer} {}
    // copy to internal buffer at most remaining_space bytes
    size_t copy_from(const char* data, size_t size);
    // read in as much as possible from fd
    struct io_stat read_all_from(int fd);
    // write out as much as possible to fd; never set has_eof
    struct io_stat write_all_to(int fd);
    void dump_to(Filebuf* filebuf);
    size_t data_size() {
        return (start_ptr <= end_ptr) ? \
            end_ptr - start_ptr : sizeof(buffer) - (start_ptr - end_ptr);
    }
    // return value in range [0, SOCK_IO_BUF_SIZE)
    size_t remaining_space() { return sizeof(buffer) - 1 - data_size(); }

 private:
    char buffer[SOCK_IO_BUF_SIZE];  // last byte don't store data
    char* start_ptr;                // points to data start
    char* end_ptr;                  // points next to data end
};

#endif  // SRC_BUFFER_H_
