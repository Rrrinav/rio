module;

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <utility>
#include <expected>
#include <format>

export module rio:file;

import :handle;
import :utils;

namespace rio
{

export enum class f_mode : int
{
    none      = 0,
    read      = O_RDONLY,
    write     = O_WRONLY,
    rw        = O_RDWR,
    create    = O_CREAT,
    truncate  = O_TRUNC,
    append    = O_APPEND,
    cloexec   = O_CLOEXEC,
    read_only = read | cloexec,
    write_new = write | create | truncate | cloexec,
    write_app = write | create | append | cloexec
};

constexpr f_mode operator|(f_mode lhs, f_mode rhs) { return static_cast<f_mode>(static_cast<int>(lhs) | static_cast<int>(rhs)); }
constexpr f_mode operator&(f_mode lhs, f_mode rhs) { return static_cast<f_mode>(static_cast<int>(lhs) & static_cast<int>(rhs)); }
constexpr bool has(f_mode subject, f_mode flag)    { return (static_cast<int>(subject) & static_cast<int>(flag)) == static_cast<int>(flag); }

export struct file
{
    rio::handle fd{};

    file() = default;
    explicit file(rio::handle &&h) : fd(std::move(h)) {}

    static auto open(const char *path, f_mode flags = f_mode::read_only) -> result<file>;
    static auto attach(int raw_fd) -> file;

    explicit operator bool() const;
};

auto file::open(const char *path, f_mode m) -> result<file>
{
    // 0644 is read/write for owner, read for group/others
    int f = ::open(path, static_cast<int>(m), 0644);

    if (f == -1)
    {
        return std::unexpected(Err{errno, std::format("Failed to open file:'{}'.", std::string(path))});
    }

    return file::attach(f);
}

auto file::attach(int raw_fd) -> file
{
    return file{rio::handle(raw_fd)};
}

file::operator bool() const
{
    return static_cast<bool>(fd);
}

}  // namespace rio
