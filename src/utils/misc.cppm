module;
export module rio:utils.misc;

import std;

namespace rio::util {
export std::string escape_string(std::string_view input)
{
    std::string result;
    result.reserve(input.size() + input.size() / 4);

    for (char c : input) {
        switch (c) {
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\"':
            result += "\\\"";
            break;
        default:
            result += c;
            break;
        }
    }

    return result;
}

export auto split_once(std::string_view str, std::string_view delim) -> std::optional<std::pair<std::string_view, std::string_view>>
{
    if (auto pos = str.find(delim); pos != std::string_view::npos) {
        return std::pair{str.substr(0, pos), str.substr(pos + delim.length())};
    }
    return std::nullopt;
}

export auto split(std::string_view str, char delim) -> std::vector<std::string_view>
{
    std::vector<std::string_view> result;
    std::size_t start = 0;
    std::size_t end = str.find(delim);

    while (end != std::string_view::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delim, start);
    }
    result.push_back(str.substr(start));

    return result;
}

export auto trim(std::string_view str) -> std::string_view
{
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";

    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}
}; // namespace rio::util

