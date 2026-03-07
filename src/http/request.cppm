module;

export module rio:http.request;

import std;

namespace rio::http::v1_1 {

export enum class method { get, post, put, del, patch, options, unknown };

export const char *method_to_str(method m)
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
    default:
        return "UNKNOWN";
    }
}

export struct header
{
    std::string name;
    std::string value;
};

export struct request
{
    method method{method::unknown};
    std::string path;
    std::string version;
    std::vector<header> headers;
    std::string body;

    auto get_header(std::string_view key) const -> std::optional<std::string_view>
    {
        for (const auto &h : headers) {
            if (h.name == key) {
                return h.value;
            }
        }
        return std::nullopt;
    }
};

export enum class Parse_state { request_line, headers, body, complete, error };

} // namespace rio::http::v1_1
