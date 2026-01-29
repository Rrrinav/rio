#include <unistd.h>
import rio;
import std;

int main()
{
    // Handles are basically fundamental types of rio and a wrapper over FDs.
    // You can initialize them like this using any file descriptors.
    // rio::handle err(STDERR_FILENO);
    rio::handle err{STDERR_FILENO};
    rio::handle out{STDOUT_FILENO};

    rio::handle in{STDIN_FILENO};

    // These are implicitly convertible to int but not copyable as handles and close fd using RAII.
    // so int q = out; is possible but auto q = out; & rio::handle q = out; isnt.
    // Because this makes ownership of files difficult.

    // **** Destructor has if (fd < 0) check so, it will close only if fd is valid. Tough choice but I like this more. *****

    /*
        There are three functions defined for handles in rio.

        Reads all the data till EOF or till size of span into the buffer
        export auto read(const rio::handle &h, std::span<char> buf) -> result<std::size_t>

        Reads all the data till EOF into a string
        export auto read(const rio::handle& h) -> result<std::string>

        Reads all the data till '\n' into a string, excluding '\n'.
        export auto read_line(const rio::handle& fd) -> result<std::string>

        Reads all the data from a buffer into a handle.
        export auto write(const rio::handle &h, std::span<const char> data) -> result<std::size_t>
    */

    if (auto res = rio::io::write(out, "Hello from rio, want to say something?\nSay: "); !res)
    {
        /*
            return types of most functions is result<T>, which is,
            std::expected<T, Err>
            and Err is { std::error_code code; std::string context; }
            and it is formattable using std::format and std::println
            I tried to make most types formattable.
        */
        // std::println("{}", res.error().message());
        std::println("{}", res.error());
        return 1;
    }

    std::string input;
    if (auto res = rio::io::read_line(in); !res)
    {
        std::println("{}", res.error());
        return 1;
    }
    else input = std::move(*res);

    // We dont expect stdout, in and err to fail generally.
    // I havent marked these functions [[nodiscard]] for programmer sanity.
    rio::io::write(out, std::format("You said: {}, thanks\n", input));
    return 0;
}
