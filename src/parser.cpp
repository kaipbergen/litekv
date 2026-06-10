#include "parser.h"
#include <algorithm>
#include <charconv>

namespace litekv {

Command Parser::parse(std::string_view input) {
    Command cmd;

    if (input.empty()) return cmd;

    // RESP protocol: *<count>\r\n$<len>\r\n<data>\r\n...
    if (input[0] == '*') {
        size_t pos = 0;

        // Parse *<count>
        size_t newline = input.find("\r\n", pos);
        if (newline == std::string_view::npos) return cmd;

        int count = 0;
        std::from_chars(input.data() + 1, input.data() + newline, count);
        pos = newline + 2;

        for (int i = 0; i < count; i++) {
            // Skip $<len>
            newline = input.find("\r\n", pos);
            if (newline == std::string_view::npos) break;
            pos = newline + 2;

            // Read data
            newline = input.find("\r\n", pos);
            if (newline == std::string_view::npos) break;

            std::string_view token = input.substr(pos, newline - pos);
            pos = newline + 2;

            if (i == 0) {
                cmd.name = std::string(token);
                std::transform(cmd.name.begin(), cmd.name.end(),
                               cmd.name.begin(), ::toupper);
            } else {
                cmd.args.emplace_back(token);
            }
        }
        return cmd;
    }

    // Plain text fallback
    size_t pos = 0;
    bool first = true;
    while (pos < input.size()) {
        while (pos < input.size() && input[pos] == ' ') pos++;
        if (pos >= input.size()) break;

        size_t end = pos;
        while (end < input.size() && input[end] != ' ' &&
               input[end] != '\r' && input[end] != '\n') end++;

        std::string_view token = input.substr(pos, end - pos);
        if (!token.empty()) {
            if (first) {
                cmd.name = std::string(token);
                std::transform(cmd.name.begin(), cmd.name.end(),
                               cmd.name.begin(), ::toupper);
                first = false;
            } else {
                cmd.args.emplace_back(token);
            }
        }
        pos = end;
    }
    return cmd;
}

std::string Parser::ok_response() { return "+OK\r\n"; }

std::string Parser::error_response(std::string_view msg) {
    return "-ERR " + std::string(msg) + "\r\n";
}

std::string Parser::bulk_response(std::string_view value) {
    return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

std::string Parser::null_response() { return "$-1\r\n"; }

std::string Parser::integer_response(int value) {
    return ":" + std::to_string(value) + "\r\n";
}

} // namespace litekv