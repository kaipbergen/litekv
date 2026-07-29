#include "third_party/catch.hpp"
#include "parser.h"

using namespace litekv;

TEST_CASE("parses a simple RESP array command", "[parser]") {
    Command cmd = Parser::parse("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(cmd.name == "SET");
    REQUIRE(cmd.args.size() == 2);
    REQUIRE(cmd.args[0] == "foo");
    REQUIRE(cmd.args[1] == "bar");
}

TEST_CASE("uppercases the command name but not its arguments", "[parser]") {
    Command cmd = Parser::parse("*2\r\n$3\r\nget\r\n$3\r\nFoo\r\n");
    REQUIRE(cmd.name == "GET");
    REQUIRE(cmd.args[0] == "Foo");
}

TEST_CASE("parses a command with no arguments", "[parser]") {
    Command cmd = Parser::parse("*1\r\n$4\r\nPING\r\n");
    REQUIRE(cmd.name == "PING");
    REQUIRE(cmd.args.empty());
}

TEST_CASE("empty input yields an empty command", "[parser]") {
    Command cmd = Parser::parse("");
    REQUIRE(cmd.name.empty());
    REQUIRE(cmd.args.empty());
}

TEST_CASE("truncated array header returns an empty command", "[parser]") {
    Command cmd = Parser::parse("*3\r\n$3\r\nSET\r\n");
    REQUIRE(cmd.name == "SET");
    REQUIRE(cmd.args.empty());
}

TEST_CASE("missing trailing CRLF on the last token stops parsing early", "[parser]") {
    Command cmd = Parser::parse("*2\r\n$3\r\nGET\r\n$3\r\nfoo");
    REQUIRE(cmd.name == "GET");
    REQUIRE(cmd.args.empty());
}

TEST_CASE("bulk string values may contain spaces", "[parser]") {
    Command cmd = Parser::parse("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$11\r\nhello world\r\n");
    REQUIRE(cmd.args[1] == "hello world");
}

TEST_CASE("falls back to plain text parsing for inline commands", "[parser]") {
    Command cmd = Parser::parse("SET foo bar\r\n");
    REQUIRE(cmd.name == "SET");
    REQUIRE(cmd.args.size() == 2);
    REQUIRE(cmd.args[0] == "foo");
    REQUIRE(cmd.args[1] == "bar");
}

TEST_CASE("plain text parsing collapses repeated spaces", "[parser]") {
    Command cmd = Parser::parse("GET   foo  \r\n");
    REQUIRE(cmd.name == "GET");
    REQUIRE(cmd.args.size() == 1);
    REQUIRE(cmd.args[0] == "foo");
}

TEST_CASE("ok_response encodes a simple string", "[parser]") {
    REQUIRE(Parser::ok_response() == "+OK\r\n");
}

TEST_CASE("error_response encodes an error", "[parser]") {
    REQUIRE(Parser::error_response("bad thing") == "-ERR bad thing\r\n");
}

TEST_CASE("bulk_response encodes length-prefixed data", "[parser]") {
    REQUIRE(Parser::bulk_response("hello") == "$5\r\nhello\r\n");
}

TEST_CASE("bulk_response handles empty strings", "[parser]") {
    REQUIRE(Parser::bulk_response("") == "$0\r\n\r\n");
}

TEST_CASE("null_response encodes a RESP nil", "[parser]") {
    REQUIRE(Parser::null_response() == "$-1\r\n");
}

TEST_CASE("integer_response encodes negative and positive values", "[parser]") {
    REQUIRE(Parser::integer_response(42) == ":42\r\n");
    REQUIRE(Parser::integer_response(-7) == ":-7\r\n");
}

TEST_CASE("array_response mixes values and nils", "[parser]") {
    std::vector<std::optional<std::string>> values = {std::string("a"), std::nullopt, std::string("c")};
    REQUIRE(Parser::array_response(values) == "*3\r\n$1\r\na\r\n$-1\r\n$1\r\nc\r\n");
}

TEST_CASE("encode_command round-trips through parse", "[parser]") {
    std::string encoded = Parser::encode_command({"SET", "foo", "bar"});
    REQUIRE(encoded == "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");

    Command cmd = Parser::parse(encoded);
    REQUIRE(cmd.name == "SET");
    REQUIRE(cmd.args[0] == "foo");
    REQUIRE(cmd.args[1] == "bar");
}
