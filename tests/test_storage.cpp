#define CATCH_CONFIG_MAIN
#include "third_party/catch.hpp"
#include "storage.h"

#include <cstdio>
#include <thread>
#include <chrono>

using namespace litekv;

namespace {
std::string temp_aof_path(const char* name) {
    return std::string("/tmp/litekv_test_") + name + ".aof";
}
}

TEST_CASE("set and get a value", "[storage]") {
    auto path = temp_aof_path("set_get");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    auto val = storage.get("foo");
    REQUIRE(val.has_value());
    REQUIRE(val.value() == "bar");
}

TEST_CASE("get on missing key returns nullopt", "[storage]") {
    auto path = temp_aof_path("missing");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.get("nope").has_value());
}

TEST_CASE("del removes a key", "[storage]") {
    auto path = temp_aof_path("del");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    REQUIRE(storage.del("foo"));
    REQUIRE_FALSE(storage.exists("foo"));
    REQUIRE_FALSE(storage.del("foo"));
}

TEST_CASE("exists reflects presence and expiry", "[storage]") {
    auto path = temp_aof_path("exists");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.exists("foo"));
    storage.set("foo", "bar");
    REQUIRE(storage.exists("foo"));

    storage.set("ephemeral", "v", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE_FALSE(storage.exists("ephemeral"));
}

TEST_CASE("incrby increments and decrements integer values", "[storage]") {
    auto path = temp_aof_path("incrby");
    std::remove(path.c_str());
    Storage storage(path);

    auto v1 = storage.incrby("counter", 1);
    REQUIRE(v1.has_value());
    REQUIRE(v1.value() == 1);

    auto v2 = storage.incrby("counter", 5);
    REQUIRE(v2.has_value());
    REQUIRE(v2.value() == 6);

    auto v3 = storage.incrby("counter", -10);
    REQUIRE(v3.has_value());
    REQUIRE(v3.value() == -4);
}

TEST_CASE("incrby fails on non-integer value", "[storage]") {
    auto path = temp_aof_path("incrby_bad");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("notanumber", "hello");
    auto result = storage.incrby("notanumber", 1);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ttl reports -1 for no expiry and -2 for missing key", "[storage]") {
    auto path = temp_aof_path("ttl");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.ttl("missing") == -2);
    storage.set("persistent", "v");
    REQUIRE(storage.ttl("persistent") == -1);
}

TEST_CASE("append creates key when missing and returns new length", "[storage]") {
    auto path = temp_aof_path("append_new");
    std::remove(path.c_str());
    Storage storage(path);

    auto len = storage.append("greeting", "Hello");
    REQUIRE(len == 5);
    REQUIRE(storage.get("greeting").value() == "Hello");
}

TEST_CASE("append concatenates onto existing value", "[storage]") {
    auto path = temp_aof_path("append_existing");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("greeting", "Hello");
    auto len = storage.append("greeting", "World");
    REQUIRE(len == 10);
    REQUIRE(storage.get("greeting").value() == "HelloWorld");
}

TEST_CASE("flush clears all keys", "[storage]") {
    auto path = temp_aof_path("flush");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("a", "1");
    storage.set("b", "2");
    REQUIRE(storage.size() == 2);
    storage.flush();
    REQUIRE(storage.size() == 0);
}
