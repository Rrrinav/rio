module;

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module rio:file;

import :handle;
import :utils.result;

import std;

namespace rio {

export enum class f_mode : int {
    none = 0,
    read = O_RDONLY,
    write = O_WRONLY,
    rw = O_RDWR,
    create = O_CREAT,
    truncate = O_TRUNC,
    append = O_APPEND,
    cloexec = O_CLOEXEC,
    read_only = read | cloexec,
    write_new = write | create | truncate | cloexec,
    write_app = write | create | append | cloexec
};

export constexpr f_mode operator|(f_mode lhs, f_mode rhs)
{
    return static_cast<f_mode>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
export constexpr f_mode operator&(f_mode lhs, f_mode rhs)
{
    return static_cast<f_mode>(static_cast<int>(lhs) & static_cast<int>(rhs));
}
constexpr bool has(f_mode subject, f_mode flag)
{
    return (static_cast<int>(subject) & static_cast<int>(flag)) == static_cast<int>(flag);
}

// Typestate file handle: the open/closed distinction lives in the type,
// so use-after-close, double-close and operating on an empty handle are
// compile errors instead of runtime -1 checks.
//
//   file<file_state::open>  — always a live fd; closes itself on drop.
//   file<file_state::closed> — empty token; only close() produces one.
//
// Ownership is move-only throughout. The only raw-fd crossings are the
// explicitly-marked attach()/detatch(); everything else is typed.
export enum class file_state { closed, open };

export template <file_state S>
struct file;

export template <>
struct file<file_state::closed>
{
    file() = default;

    file(const file &) = delete;
    file &operator=(const file &) = delete;
    file(file &&) = default;
    file &operator=(file &&) = default;
};

export template <>
struct file<file_state::open>
{
    // Capability marker: only readable states carry this, so the generic
    // io/fut read/write overloads reject closed handles at compile time.
    using readable_tag = void;

    rio::handle fd{};

    file() = delete;
    explicit file(rio::handle &&h) noexcept : fd(std::move(h))
    {}

    file(const file &) = delete;
    file &operator=(const file &) = delete;
    file(file &&) = default;
    file &operator=(file &&) = default;
    // handle member closes the fd on drop; nothing to do here.

    static auto open(const char *path, f_mode flags = f_mode::read_only) -> result<file>;
    // Unsafe entry: takes ownership of a raw fd with no validation.
    static auto attach(int raw_fd) -> file;

    // Consuming transitions: after either call the object is moved-from
    // and unusable, by construction.
    auto close() && -> file<file_state::closed>;
    // Unsafe escape: hands the raw fd out and disarms the destructor.
    auto detatch() && -> int;

    auto native_handle() const -> int
    {
        return fd.native_handle();
    }
};

auto file<file_state::open>::open(const char *path, f_mode m) -> result<file>
{
    // 0644 is read/write for owner, read for group/others
    int f = ::open(path, static_cast<int>(m), 0644);

    if (f == -1) {
        return std::unexpected(rio::Err::sys(std::format("Failed to open file:'{}'.", std::string(path))));
    }

    return attach(f);
}

auto file<file_state::open>::attach(int raw_fd) -> file
{
    return file{rio::handle(raw_fd)};
}

auto file<file_state::open>::close() && -> file<file_state::closed>
{
    fd.close();
    return file<file_state::closed>{};
}

auto file<file_state::open>::detatch() && -> int
{
    return fd.detatch();
}

} // namespace rio
