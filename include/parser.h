#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace litekv {

struct Command {
    std::string name;
    std::vector<std::string> args;
};

class Parser {
public:
    static Command parse(std::string_view input);
    // Returns the byte length of the first complete command in `input` (mirroring
    // the same skip/consume logic as parse()), or 0 if `input` doesn't yet contain
    // one full command. Used to split pipelined commands out of a byte stream.
    static size_t command_length(std::string_view input);
    static std::string ok_response();
    static std::string error_response(std::string_view msg);
    static std::string bulk_response(std::string_view value);
    static std::string null_response();
    static std::string integer_response(long long value);
    static std::string array_response(const std::vector<std::optional<std::string>>& values);
    static std::string encode_command(const std::vector<std::string>& tokens);
};

} // namespace litekv