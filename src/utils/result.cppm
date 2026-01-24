module;
#include <cerrno>
export module rio:utils.result;

import std;

namespace rio {

export struct Err
{
    std::error_code code;
    std::string context;

    Err() = default;

    Err(std::error_code ec, std::string msg = "") : code(ec), context(std::move(msg)) {}

    Err(int errno_val, std::string msg = "") : code(std::make_error_code(static_cast<std::errc>(errno_val))), context(std::move(msg)) {}

    Err(std::errc err_c, std::string msg = "") : code(std::make_error_code(err_c)), context(std::move(msg)) {}

    [[nodiscard]]
    std::string message() const
    {
        if (context.empty())
            return code.message();
        return std::format("{}\n    err({}): {}.", context, code.value(), code.message());
    }

    static auto sys(std::string msg)
    {
        return Err(errno, std::move(msg));
    }

    static auto app(std::errc err_c, std::string msg)
    {
        return Err(err_c, std::move(msg));
    }
};

export template<typename T>
using result = std::expected<T, Err>;

} // namespace rio

export template <>
struct std::formatter<rio::Err> : std::formatter<std::string>
{
    auto format(const rio::Err &err, std::format_context &ctx) const 
    { return std::formatter<std::string>::format(err.message(), ctx); }
};
