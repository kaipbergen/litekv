#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace litekv {

struct Command {
    std::string name;
    std::vector<std::string> args;
};

class Parser {
public:
    static Command parse(std::string_view input);
    static std::string ok_response();
    static std::string error_response(std::string_view msg);
    static std::string bulk_response(std::string_view value);
    static std::string null_response();
    static std::string integer_response(long long value);
    static std::string encode_command(const std::vector<std::string>& tokens);
};

} // namespace litekv