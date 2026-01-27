module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

export module rio:handle;

namespace rio {

export struct handle
{
    int fd = -1;

    // Constructors
    handle() = default;
    explicit handle(int f) : fd(f) {}

    handle(const handle &) = delete;
    handle &operator=(const handle &) = delete;

    handle(handle &&other) noexcept : fd(other.fd) { other.fd = -1; }
    handle &operator=(handle &&other) noexcept;
    ~handle() { close(); }

    // Minimal methods
    void close();
    explicit operator bool() const;
    operator int() const { return fd; }
    int native_handle() const;
};

// Definitions

handle & handle::operator=(handle &&other) noexcept
{
    if (this != &other)
    {
        close();
        fd = other.fd;
        other.fd = -1;
    }
    return *this;
}

void handle::close()
{
    if (fd != -1)
    {
        ::close(fd);
        fd = -1;
    }
}
handle::operator bool() const { return fd != -1; }
int handle::native_handle() const { return fd; }
} // namespace rio
