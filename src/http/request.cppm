module;

export module rio:http.request;

import std;

namespace rio::http::v1_1 {

export enum class method : std::uint8_t {
    get     = 0,
    post    = 1,
    put     = 2,
    del     = 3,
    patch   = 4,
    options = 5,
    unknown = 6,
};

export inline constexpr std::size_t method_count = 6; // excludes 'unknown'

export constexpr const char *method_to_str(method m) noexcept
{
    switch (m) {
    case method::get:
        return "GET";
    case method::post:
        return "POST";
    case method::put:
        return "PUT";
    case method::del:
        return "DELETE";
    case method::patch:
        return "PATCH";
    case method::options:
        return "OPTIONS";
    default:
        return "UNKNOWN";
    }
}

export struct header
{
    std::string name;
    std::string value;
};

export template <std::size_t InlineHeaders = 16>
struct basic_request
{
    method method{method::unknown};
    std::string path{};
    std::string version{};
    std::string body{};

    //  inline header storage
    std::size_t header_count{0};
    std::array<header, InlineHeaders> inline_headers{};
    std::vector<header> extra_headers{}; // populated only if > InlineHeaders

    void push_header(std::string name, std::string value)
    {
        if (header_count < InlineHeaders) {
            inline_headers[header_count] = {std::move(name), std::move(value)};
        } else {
            extra_headers.push_back({std::move(name), std::move(value)});
        }
        ++header_count;
    }

    // Case-insensitive lookup (ASCII only — correct for HTTP field names).
    [[nodiscard]]
    auto get_header(std::string_view key) const noexcept -> std::optional<std::string_view>
    {
        auto iequal = [](std::string_view a, std::string_view b) noexcept -> bool {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if ((a[i] | 0x20) != (b[i] | 0x20))
                    return false;
            return true;
        };

        const auto n = std::min(header_count, InlineHeaders);
        for (std::size_t i = 0; i < n; ++i)
            if (iequal(inline_headers[i].name, key))
                return inline_headers[i].value;

        for (const auto &h : extra_headers)
            if (iequal(h.name, key))
                return h.value;

        return std::nullopt;
    }

    // Reuse the request object across connections — avoids re-allocating body.
    void clear() noexcept
    {
        method = method::unknown;
        path.clear();
        version.clear();
        body.clear(); // keeps the capacity
        header_count = 0;
        extra_headers.clear();
    }
};

export using request = basic_request<>;

// Kept here so parser.cppm can import it without an extra module dep.
export enum class Parse_state : std::uint8_t {
    request_line,
    headers,
    body,
    complete,
    error,
};

} // namespace rio::http::v1_1
