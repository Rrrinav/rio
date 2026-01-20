module;

#include <source_location>
#include <unistd.h>
#include <cerrno>
#include <expected>
#include <format>

export module rio;

export import rio.io;
export import rio.utils;
export import rio.handle;
export import rio.file;
export import rio.socket;
export import rio.context;

namespace rio {
export auto kill(rio::handle &h) -> void
{
    if (h.fd != -1)
    {
        ::close(h.fd);
        h.fd = -1;
    }
}

export [[nodiscard]]
auto try_kill(rio::handle &h, std::source_location loc = std::source_location::current()) -> result<void>
{
    if (h.fd == -1)
        return {};

    int fd = h.fd;
    h.fd = -1;

    if (::close(fd) == -1)
        return std::unexpected(Err{errno, std::format("{}:{}: Couldn't close handle (fd = {}).", loc.file_name(), loc.line(), h.fd)});
    return {};
}
}
