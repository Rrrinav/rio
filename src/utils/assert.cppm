module;

#include <string_view>
#include <source_location>
#include <print>
#include <cstdlib>

export module rio.utils:assert;

namespace rio::assrt
{

// Helper: Internal linkage (not exported), keeps module clean
void log(std::string_view msg, const std::source_location &loc)
{
    std::println(stderr, "\n{}:{}: [assert failed] {}", loc.file_name(), loc.line(), msg);
}

export inline auto that( bool condition, std::string_view msg = "Check failed", const std::source_location loc = std::source_location::current()) -> void
{
#ifndef NDEBUG
    if (!condition)
    {
        log(msg, loc);
        std::abort();
    }
#endif
}

export  inline auto ensure( bool condition, std::string_view msg = "Check failed", const std::source_location loc = std::source_location::current()) -> void
{
    if (!condition)
    {
        log(msg, loc);
        std::abort();
    }
}
}  // namespace rio::assrt
