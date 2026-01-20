module;

#include <string>
#include <span>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <cerrno>

export module rio:io;

import :file;

export namespace rio::io {

inline long long get_native_size(int fd)
{
    struct stat st;
    if (::fstat(fd, &st) < 0)
        return -1;
    if (S_ISREG(st.st_mode))
        return st.st_size;
    if (S_ISBLK(st.st_mode))
    {
        unsigned long long bytes = 0;
        if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        return static_cast<long long>(bytes);
    }
    return -1;
}

// --- Operations ---
inline size_t read(const rio::file &f, std::span<char> buffer)
{
    if (!f)
        return 0;
    size_t total = 0;
    while (total < buffer.size())
    {
        ssize_t n = ::read(f.fd, buffer.data() + total, buffer.size() - total);
        if (n == -1)
        {
            if (errno == EINTR)
                continue;
            return total;
        }
        if (n == 0)
            break;
        total += n;
    }
    return total;
}

inline bool read(const rio::file &f, std::string &s)
{
    if (!f)
        return false;
    long long len = get_native_size(f.fd);
    if (len < 0)
        return false;

    try
    {
        s.resize(static_cast<size_t>(len));
    }
    catch (...)
    {
        return false;
    }

    size_t bytes_read = read(f, std::span<char>(s));
    if (bytes_read != static_cast<size_t>(len))
        s.resize(bytes_read);
    return bytes_read > 0 || len == 0;
}

inline bool write(const rio::file &f, std::span<const char> data)
{
    if (!f)
        return false;
    size_t total = 0;
    while (total < data.size())
    {
        ssize_t n = ::write(f.fd, data.data() + total, data.size() - total);
        if (n == -1)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        total += n;
    }
    return true;
}

inline bool write(const rio::file &f, std::string_view str) { return write(f, std::span<const char>(str.data(), str.size())); }

inline void sync(const rio::file &f)
{
    if (f)
        ::fsync(f.fd);
}
}  // namespace rio::io
