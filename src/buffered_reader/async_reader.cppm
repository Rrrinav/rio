module;

#include <cstring>

export module rio:buff_reader_async;

import std;
import :io;
import :context;
import :futures;
import :buff_reader;
import :fut.io;

namespace rio {

template <typename Stream>
struct async_stream_traits
{
    static auto read(rio::context &ctx, Stream &s, std::span<char> buffer) { return rio::fut::read(ctx, s, buffer); }
};

template <typename T>
concept AsyncReadable = requires(rio::context &ctx, T &t, std::span<char> buf) {
    { async_stream_traits<T>::read(ctx, t, buf) };
};

// Forward Declaration
export template <AsyncReadable Stream, std::size_t BufferSize>
class Async_buffered_reader;

    namespace detail {

    template <typename Reader>
    struct Peek_op_impl
    {
        Reader *reader;

        using Read_fut = decltype(async_stream_traits<typename Reader::stream_type>::read(std::declval<rio::context &>(), std::declval<typename Reader::stream_type &>(), std::span<char>{}));

        std::optional<Read_fut> pending_read{};

        auto poll() -> rio::fut::res<typename Reader::view_type>
        {
            while (true)
            {
                // 1. Check if we have data
                if (reader->cursor_ < reader->valid_bytes_)
                    return rio::fut::res<typename Reader::view_type>::ready({reader->buffer_.start() + reader->cursor_, reader->valid_bytes_ - reader->cursor_});

                // 2. Buffer empty. Need to refill.
                if (!pending_read)
                {
                    // Reset buffer state (shift logic not needed since it's empty)
                    reader->cursor_ = 0;
                    reader->valid_bytes_ = 0;

                    // Start Async Read
                    pending_read.emplace(async_stream_traits<typename Reader::stream_type>::read(
                        reader->ctx_, reader->stream_, std::span<char>(
                            reinterpret_cast<char *>(reader->buffer_.start()),
                            reader->buffer_.capacity())
                        )
                    );
                }

                // 3. Poll underlying IO
                auto r = pending_read->poll();

                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<typename Reader::view_type>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<typename Reader::view_type>::error(r.err);

                // 4. IO Complete
                size_t n = *r.value;
                pending_read.reset();  // Clear future

                // EOF: Return empty view
                if (n == 0)
                    return rio::fut::res<typename Reader::view_type>::ready({});

                reader->valid_bytes_ = n;
                // Loop back to Step 1 to return the view
            }
        }
    };

    template <typename Reader>
    struct Read_till_op_impl
    {
        Reader *reader;
        char delimiter;
        std::string result{};
        Peek_op_impl<Reader> peek_state;

        Read_till_op_impl(Reader *r, char d) : reader(r), delimiter(d), peek_state{r} {}

        auto poll() -> rio::fut::res<std::string>
        {
            while (true)
            {
                // 1. Get View (Async Peek)
                auto r = peek_state.poll();

                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<std::string>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<std::string>::error(r.err);

                auto view = *r.value;

                // 2. Handle EOF
                if (view.empty())
                    return rio::fut::res<std::string>::ready(std::move(result));

                // 3. Scan for delimiter
                const void *match = std::memchr(view.data(), delimiter, view.size());

                if (match)
                {
                    auto *match_ptr = static_cast<const std::uint8_t *>(match);
                    std::size_t length = match_ptr - view.data();

                    result.append(reinterpret_cast<const char *>(view.data()), length);
                    reader->advance(length + 1);  // Skip delimiter

                    return rio::fut::res<std::string>::ready(std::move(result));
                }
                else
                {
                    // Not found, consume all and continue
                    result.append(reinterpret_cast<const char *>(view.data()), view.size());
                    reader->advance(view.size());
                    // Loop continues... Peek_op will trigger refill
                }
            }
        }
    };

    template <typename T, typename Reader>
    struct Load_op
    {
        Reader *reader;
        Peek_op_impl<Reader> peek_state;

        // To handle struct split across buffers, we might need a temp buffer.
        // For simple implementation, we assume T fits in one buffer chunk.
        // Or we just loop filling a stack buffer.
        std::size_t bytes_copied = 0;
        std::array<std::byte, sizeof(T)> temp_storage;

        Load_op(Reader *r) : reader(r), peek_state{r} {}

        auto poll() -> rio::fut::res<T>
        {
            static_assert(std::is_trivially_copyable_v<T>, "Async Load requires POD types");

            while (bytes_copied < sizeof(T))
            {
                // 1. Get View
                auto r = peek_state.poll();

                if (r.state == rio::fut::status::pending)
                    return rio::fut::res<T>::pending();
                if (r.state == rio::fut::status::error)
                    return rio::fut::res<T>::error(r.err);

                auto view = *r.value;

                // 2. Handle EOF
                if (view.empty())
                    return rio::fut::res<T>::error(std::make_error_code(std::errc::bad_message));

                // 3. Copy what we can
                size_t needed = sizeof(T) - bytes_copied;
                size_t to_copy = std::min(needed, view.size());

                std::memcpy(temp_storage.data() + bytes_copied, view.data(), to_copy);

                bytes_copied += to_copy;
                reader->advance(to_copy);
            }

            // 4. Done
            T val;
            std::memcpy(&val, temp_storage.data(), sizeof(T));
            return rio::fut::res<T>::ready(val);
        }
    };

    }  // namespace detail

export template <AsyncReadable Stream, std::size_t BufferSize = 0>
class Async_buffered_reader
{
public:
    using value_type = std::uint8_t;
    using size_type = std::size_t;
    using view_type = std::span<const value_type>;
    using stream_type = Stream;

    rio::context &ctx_;
    Stream &stream_;
    rio::buff::detail::Storage_policy<BufferSize> buffer_;
    size_type cursor_{0};
    size_type valid_bytes_{0};

public:
    explicit Async_buffered_reader(rio::context &ctx, Stream &stream, size_type explicit_size = 4096)
        : ctx_(ctx), stream_(stream), buffer_(explicit_size) {}

    /// \brief Returns a future that resolves to a view of the buffer.
    /// (Async because it might need to refill from disk/net)
    auto peek()
    {
        return rio::Future(detail::Peek_op_impl<Async_buffered_reader>{this}, [](auto &op) { return op.poll(); });
    }

    /// \brief Advance cursor manually (Sync operation).
    void advance(size_type n) { cursor_ += n; }
};

    namespace fut::buff {

    export template <typename Stream, size_t N>
    auto read_till(Async_buffered_reader<Stream, N> &reader, char delimiter)
    {
        using Op = detail::Read_till_op_impl<Async_buffered_reader<Stream, N>>;
        return rio::Future(Op(&reader, delimiter), [](Op &op) { return op.poll(); });
    }

    export template <typename Stream, size_t N>
    auto read_line(Async_buffered_reader<Stream, N> &reader)
    {
        return read_till(reader, '\n');
    }

    export template <typename T, typename Stream, size_t N>
    auto load(Async_buffered_reader<Stream, N> &reader)
    {
        using Op = detail::Load_op<T, Async_buffered_reader<Stream, N>>;
        return rio::Future(Op(&reader), [](Op &op) { return op.poll(); });
    }

    } // namespace fut::buff
} // namespace rio
