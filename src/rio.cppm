module;

#include <cerrno>
#include <unistd.h>

export module rio;

import std;

export import :io;
export import :io.tcp_socket;
export import :io.udp_socket;
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
export import :fut.udp.io;
export import :fut.file.io;
export import :buff_reader;
export import :buff_reader_async;
export import :http;

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
    if (h.fd == -1) {
        return {};
    }

    int fd = h.fd;
    h.fd = -1;

    if (::close(fd) == -1) {
        return std::unexpected(Err{errno, std::format("{}:{}: Couldn't close handle (fd = {}).", loc.file_name(), loc.line(), h.fd)});
    }

    return {};
}

} // namespace rio

namespace rio {
using namespace std::string_view_literals;
namespace detail {
consteval std::uint8_t parse_u8(std::string_view sv)
{
    std::uint8_t res = 0;
    for (char c : sv) {
        res = res * 10 + (c - '0');
    }
    return res;
}
}

export struct version
{
    // Single source of truth
    static constexpr std::string_view ver_str = "0.0.0"sv;

private:
    // Helper indices to find the delimiters at compile-time
    static constexpr auto _first_dot = ver_str.find('.');
    static constexpr auto _second_dot = ver_str.find('.', _first_dot + 1);

public:
    static constexpr std::string_view major_str = ver_str.substr(0, _first_dot);
    static constexpr std::string_view minor_str = ver_str.substr(_first_dot + 1, _second_dot - _first_dot - 1);
    static constexpr std::string_view patch_str = ver_str.substr(_second_dot + 1);

    static constexpr std::uint8_t major = detail::parse_u8(major_str);
    static constexpr std::uint8_t minor = detail::parse_u8(minor_str);
    static constexpr std::uint8_t patch = detail::parse_u8(patch_str);
};
} // namespace rio
