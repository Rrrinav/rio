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
}; // namespace rio::util

