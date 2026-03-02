module;

#include <cerrno>
#include <unistd.h>

export module rio;

import std;

export import :io;
export import :io.tcp_socket;
export import :io.file;
export import :utils;
export import :handle;
export import :file;
export import :socket;
export import :context;
export import :runtime;
export import :asio;
export import :futures;
export import :promise;
export import :fut.task;
export import :fut.io;
export import :fut.tcp.io;
export import :fut.file.io;
export import :buff_reader;
export import :buff_reader_async;

namespace rio {
export auto kill(rio::handle &h) -> void
{
    if (h.fd != -1) {
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
} // namespace rio
