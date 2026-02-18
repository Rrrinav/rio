module;

export module rio:buff_reader;

import std;
import std.compat;

import :io;

#define __USED_IN_RIO_
#define __RIO_IO_MODULE_PRESENT_

namespace rio {

namespace buff {

/// \brief Customization point to teach Buffered_reader how to read from a specific type.
///
/// specialized this struct for your custom stream types (e.g. sockets, files).
/// By default, it calls `s.read(dest, max)`.
template <typename Stream>
struct stream_traits
{
    /// \brief Reads up to `max` bytes from `s` into `dest`.
    /// \return Number of bytes read, or std::nullopt on error/EOF signal.
    static std::optional<std::size_t> read(Stream &s, std::uint8_t *dest, std::size_t max)
    {
#if defined(__USED_IN_RIO_) && defined(__RIO_IO_MODULE_PRESENT_)
        if constexpr (requires { rio::io::read(s, std::span<char>{}); }) {
            auto *char_ptr = reinterpret_cast<char *>(dest);
            auto res = rio::io::read(s, std::span<char>(char_ptr, max));
            return res ? std::optional{ *res } : std::nullopt;
        }
#endif
        else
            return s.read(dest, max);
    }
};

/// \brief Concept ensuring a type has a valid stream_traits specialization.
template <typename T>
concept Readable = requires(T &t, std::uint8_t *ptr, std::size_t len) {
    { stream_traits<T>::read(t, ptr, len) } -> std::same_as<std::optional<std::size_t>>;
};

namespace detail {

// Internal policy to handle stack vs heap buffer allocation
export template <std::size_t N>
struct Storage_policy
{
    alignas(std::max_align_t) std::byte data_[N];
    Storage_policy(std::size_t) noexcept
    {}
    std::uint8_t *start() noexcept
    {
        return reinterpret_cast<std::uint8_t *>(data_);
    }
    std::size_t capacity() const noexcept
    {
        return N;
    }
};

export template <>
struct Storage_policy<0>
{
    std::vector<std::uint8_t> data_;
    explicit Storage_policy(std::size_t n) : data_(n)
    {}
    std::uint8_t *start() noexcept
    {
        return data_.data();
    }
    std::size_t capacity() const noexcept
    {
        return data_.size();
    }
};

template <typename Reader>
void skip_whitespace(Reader &reader)
{
    while (true) {
        auto view = reader.peek();
        if (view.empty())
            return;
        std::size_t i = 0;
        while (i < view.size()) {
            if (!std::isspace(static_cast<unsigned char>(view[i]))) {
                reader.advance(i);
                return;
            }
            ++i;
        }
        reader.advance(view.size());
    }
}
inline bool is_stopper(unsigned char c)
{
    return std::isspace(c) || c == '\0' || c == ',';
}

} // namespace detail

/// \brief Trait for loading binary data. Specialize this for custom types.
export template <typename T>
struct load_traits
{
    template <typename Reader>
    static std::expected<void, std::errc> load(Reader &r, T &out)
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::size_t n = r.read(&out, sizeof(T));
            if (n != sizeof(T))
                return std::unexpected(std::errc::bad_message);
            return {};
        } else {
            static_assert(sizeof(T) == 0, "Type is not Trivially Copyable. Specialize load_traits.");
        }
    }
};

/// \brief Trait for parsing text data. Specialize this for custom types.
export template <typename T>
struct parse_traits
{
    template <typename Reader>
    static std::expected<T, std::errc> parse(Reader &)
    {
        static_assert(sizeof(T) == 0, "Type not supported by parse. Specialize parse_traits.");
        return {};
    }
};

/// \brief Specialization for Arithmetic types (int, float, double).
/// Uses std::from_chars for high-performance, locale-independent parsing.
template <typename T>
    requires std::is_arithmetic_v<T>
struct parse_traits<T>
{
    template <typename Reader>
    static std::expected<T, std::errc> parse(Reader &reader)
    {
        detail::skip_whitespace(reader);
        auto view = reader.peek();
        if (view.empty())
            return std::unexpected(std::errc::invalid_argument); // Unexpected EOF

        const char *begin = reinterpret_cast<const char *>(view.data());
        const char *end = begin + view.size();
        T value;

        // Fast path: try parsing from current chunk
        auto res = std::from_chars(begin, end, value);

        if (res.ec == std::errc()) {
            if (res.ptr != end) {
                reader.advance(res.ptr - begin);
                return value;
            }
            // Fallthrough: Number might be split across buffer boundary
        } else if (res.ec == std::errc::invalid_argument) {
            return std::unexpected(std::errc::invalid_argument);
        }

        // Slow path: Stitch split number into temp buffer
        constexpr std::size_t MAX_LEN = 64;
        std::array<char, MAX_LEN> buf;
        std::size_t len = 0;

        while (len < MAX_LEN - 1) {
            auto chunk = reader.peek();
            if (chunk.empty())
                break;
            const char *ptr = reinterpret_cast<const char *>(chunk.data());
            if (detail::is_stopper(static_cast<unsigned char>(*ptr)))
                break;
            buf[len++] = *ptr;
            reader.advance(1);
        }

        auto res2 = std::from_chars(buf.data(), buf.data() + len, value);
        if (res2.ec == std::errc())
            return value;

        return std::unexpected(std::errc::result_out_of_range);
    }
};

/// \brief Specialization for std::string parsing.
template <>
struct parse_traits<std::string>
{
    template <typename Reader>
    static std::expected<std::string, std::errc> parse(Reader &reader)
    {
        detail::skip_whitespace(reader);

        std::string result;
        bool content_found = false;

        while (true) {
            auto view = reader.peek();
            if (view.empty())
                break;

            const char *ptr = reinterpret_cast<const char *>(view.data());
            std::size_t i = 0;
            bool found_stop = false;

            // Scan for stopper (whitespace)
            for (; i < view.size(); ++i) {
                if (detail::is_stopper(static_cast<unsigned char>(ptr[i]))) {
                    found_stop = true;
                    break;
                }
            }

            if (i > 0) {
                result.append(ptr, i);
                reader.advance(i);
                content_found = true;
            }

            if (found_stop)
                break;
        }

        if (!content_found && result.empty()) {
            // We tried to parse but hit EOF immediately after skipping whitespace
            return std::unexpected(std::errc::invalid_argument);
        }

        return result;
    }
};

/// \brief A simple wrapper to treat a std::string as a readable stream.
/// Useful for testing Buffered_reader logic without disk I/O.
export struct String_source
{
    std::string content;
    std::size_t pos = 0;

    /// \brief Factory to create a source from an existing string.
    static String_source from(std::string s)
    {
        return { std::move(s), 0 };
    }
};

/// \brief Teach Buffered_reader how to read from String_source.
template <>
struct stream_traits<String_source>
{
    static std::optional<std::size_t> read(String_source &s, std::uint8_t *dest, std::size_t max)
    {
        if (s.pos >= s.content.size())
            return std::nullopt; // EOF

        std::size_t available = s.content.size() - s.pos;
        std::size_t count = std::min(available, max);

        std::memcpy(dest, s.content.data() + s.pos, count);
        s.pos += count;
        return count;
    }
};

/// \brief A non-owning stream wrapper around a raw memory buffer.
/// ideal for parsing string literals, vectors, or mmap'd regions.
export struct Memory_source
{
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;

    /// \brief Construct from a span of bytes.
    constexpr Memory_source(std::span<const std::uint8_t> s) : data(s)
    {}

    /// \brief Construct from a string view.
    constexpr Memory_source(std::string_view sv) : data(reinterpret_cast<const std::uint8_t *>(sv.data()), sv.size())
    {}

    /// \brief Construct from a C-string.
    constexpr Memory_source(const char *s)
        : data(reinterpret_cast<const std::uint8_t *>(s), std::char_traits<char>::length(s))
    {}
};

/// \brief Teach Buffered_reader how to read from Memory_source.
template <>
struct stream_traits<Memory_source>
{
    static std::optional<std::size_t> read(Memory_source &s, std::uint8_t *dest, std::size_t max)
    {
        if (s.pos >= s.data.size())
            return std::nullopt; // EOF

        std::size_t available = s.data.size() - s.pos;
        std::size_t count = std::min(available, max);

        std::memcpy(dest, s.data.data() + s.pos, count);
        s.pos += count;

        return count;
    }
};

} // namespace buff

/// Wraps a raw stream (File, Socket, etc.) and manages an internal buffer.
/// Allows for peeking, parsing, and efficient block reading.
///
/// \tparam Stream The underlying source type (must satisfy Readable).
/// \tparam BufferSize Size of internal buffer. 0 = Dynamic (Heap), >0 = Static (Stack).
export template <buff::Readable Stream, std::size_t BufferSize = 0>
class Buffered_reader
{
public:
    using value_type = std::uint8_t;
    using size_type = std::size_t;
    using view_type = std::span<const value_type>;

    Stream &stream_;
    buff::detail::Storage_policy<BufferSize> buffer_;
    size_type cursor_{ 0 };      ///< Current read position in buffer
    size_type valid_bytes_{ 0 }; ///< Total valid bytes currently in buffer

public:
    /// \brief Construct a reader.
    /// \param stream The source stream (must outlive the reader).
    /// \param explicit_size Buffer size (only used if BufferSize template param is 0).
    explicit Buffered_reader(Stream &stream, size_type explicit_size = 4096) : stream_(stream), buffer_(explicit_size)
    {}

    /// \brief Get a zero-copy view of the current buffer.
    ///
    /// Returns a span of data currently available in memory.
    /// If the buffer is empty, it attempts to refill from the stream.
    /// \return A span of bytes. Empty if EOF.
    [[nodiscard]] view_type peek()
    {
        if (cursor_ >= valid_bytes_) {
            if (refill() == 0)
                return {};
        }
        return { buffer_.start() + cursor_, valid_bytes_ - cursor_ };
    }

    /// \brief Advance the cursor manually.
    /// Use this after consuming data via `peek()`.
    void advance(size_type n)
    {
        cursor_ += n;
    }

    /// \brief Read bytes into a destination buffer.
    ///
    /// Handles copying from the internal buffer and refilling as needed.
    /// Optimization: Large reads bypass the internal buffer entirely.
    /// \param dest Pointer to write data to.
    /// \param count Number of bytes to read.
    /// \return Total bytes read (may be less than count on EOF).
    [[nodiscard]] size_type read(void *dest, size_type count)
    {
        auto *out = static_cast<value_type *>(dest);
        size_type total_read = 0;

        while (count > 0) {
            // Bypass buffer for large reads if buffer is empty
            if (remaining() == 0 && count >= buffer_.capacity()) {
                auto res = buff::stream_traits<Stream>::read(stream_, out, count);
                if (!res || *res == 0)
                    break;
                total_read += *res;
                out += *res;
                count -= *res;
                break;
            }

            view_type chunk = peek();
            if (chunk.empty())
                break;

            size_type to_copy = std::min(count, chunk.size());
            std::memcpy(out, chunk.data(), to_copy);
            advance(to_copy);
            out += to_copy;
            count -= to_copy;
            total_read += to_copy;
        }
        return total_read;
    }

    /// \brief Bytes remaining in the current buffer chunk.
    [[nodiscard]] size_type remaining() const noexcept
    {
        return valid_bytes_ - cursor_;
    }

private:
    /// \brief Refills the buffer from the stream.
    /// Moves existing data to the start and reads new data.
    size_type refill()
    {
        size_type bytes_left = remaining();
        if (bytes_left > 0 && cursor_ > 0)
            std::memmove(buffer_.start(), buffer_.start() + cursor_, bytes_left);
        valid_bytes_ = bytes_left;
        cursor_ = 0;

        size_type available = buffer_.capacity() - valid_bytes_;
        if (available == 0)
            return 0;

        auto res = buff::stream_traits<Stream>::read(stream_, buffer_.start() + valid_bytes_, available);
        if (res) {
            valid_bytes_ += *res;
            return *res;
        }
        return 0;
    }
};

namespace buff {

/// \brief Reads from the reader until a delimiter is found.
/// \return String containing data up to the delimiter (delimiter is consumed but not returned).
export template <typename Reader>
std::string read_till(Reader &reader, char delimiter)
{
    std::string result;
    while (true) {
        auto view = reader.peek();
        if (view.empty())
            return result;

        const void *match = std::memchr(view.data(), delimiter, view.size());
        if (match) {
            auto *match_ptr = static_cast<const std::uint8_t *>(match);
            std::size_t length = match_ptr - view.data();
            result.append(reinterpret_cast<const char *>(view.data()), length);
            reader.advance(length + 1);
            return result;
        } else {
            result.append(reinterpret_cast<const char *>(view.data()), view.size());
            reader.advance(view.size());
        }
    }
}
/// \brief Reads until delimiter or until the output buffer is full.
///
/// \param reader The source reader.
/// \param delimiter The char to stop at.
/// \param out The destination buffer.
/// \return Number of bytes written to 'out', or error if buffer fills before delimiter.
export template <typename Reader>
std::size_t read_till(Reader &reader, char delimiter, std::span<char> out)
{
    std::size_t total_written = 0;

    while (true) {
        // 1. Check if we ran out of space
        if (total_written >= out.size())
            return total_written;

        // 2. Peek at data
        auto view = reader.peek();
        if (view.empty())
            return total_written;

        // 3. Search for delimiter in the current chunk
        const void *match = std::memchr(view.data(), delimiter, view.size());

        std::size_t chunk_len;
        bool found = false;

        if (match) {
            // Found it! Calculate length up to delimiter.
            auto *match_ptr = static_cast<const std::uint8_t *>(match);
            chunk_len = match_ptr - view.data();
            found = true;
        } else {
            // Not found in this chunk. Take everything.
            chunk_len = view.size();
        }

        // 4. Check if this chunk fits in remaining output space
        std::size_t space_left = out.size() - total_written;

        if (chunk_len > space_left) {
            // It doesn't fit!
            // Copy what we can, then return error.
            std::memcpy(out.data() + total_written, view.data(), space_left);
            reader.advance(space_left);
            return total_written;
        }

        // 5. Copy the data
        std::memcpy(out.data() + total_written, view.data(), chunk_len);
        total_written += chunk_len;

        // 6. Advance reader
        if (found) {
            // Consume data + delimiter
            reader.advance(chunk_len + 1);
            return total_written;
        } else {
            // Consume data only (loop to get next chunk)
            reader.advance(chunk_len);
        }
    }
}

/// \brief Load a value of type T from the stream in binary format.
///
/// For POD types (int, float, struct), performs a direct memory copy.
/// \return The loaded value or an error code.
export template <typename T, typename Reader>
[[nodiscard]] std::expected<T, std::errc> load(Reader &reader)
{
    T value;
    auto res = buff::load_traits<T>::load(reader, value);
    if (!res)
        return std::unexpected(res.error());
    return value;
}

/// \brief Parse a value of type T from text (e.g. "123", "45.6").
/// \return The parsed value or an error code.
export template <typename T, typename Reader>
[[nodiscard]] std::expected<T, std::errc> parse(Reader &reader)
{
    return buff::parse_traits<T>::parse(reader);
}

} // namespace buff

} // namespace rio
