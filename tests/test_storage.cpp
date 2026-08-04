#define CATCH_CONFIG_MAIN
#include "third_party/catch.hpp"
#include "storage.h"

#include <cstdio>
#include <thread>
#include <chrono>
#include <algorithm>

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

TEST_CASE("strlen returns value length or zero for missing key", "[storage]") {
    auto path = temp_aof_path("strlen");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.strlen("missing") == 0);
    storage.set("greeting", "hello");
    REQUIRE(storage.strlen("greeting") == 5);
}

TEST_CASE("mset sets multiple keys and mget reads them back", "[storage]") {
    auto path = temp_aof_path("mset_mget");
    std::remove(path.c_str());
    Storage storage(path);

    storage.mset({{"a", "1"}, {"b", "2"}});
    auto results = storage.mget({"a", "b", "missing"});
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].value() == "1");
    REQUIRE(results[1].value() == "2");
    REQUIRE_FALSE(results[2].has_value());
}

TEST_CASE("getset returns old value and sets new one", "[storage]") {
    auto path = temp_aof_path("getset");
    std::remove(path.c_str());
    Storage storage(path);

    auto v1 = storage.getset("foo", "bar");
    REQUIRE_FALSE(v1.has_value());
    REQUIRE(storage.get("foo").value() == "bar");

    auto v2 = storage.getset("foo", "baz");
    REQUIRE(v2.has_value());
    REQUIRE(v2.value() == "bar");
    REQUIRE(storage.get("foo").value() == "baz");
}

TEST_CASE("setnx only sets when key is absent", "[storage]") {
    auto path = temp_aof_path("setnx");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.setnx("foo", "bar"));
    REQUIRE(storage.get("foo").value() == "bar");

    REQUIRE_FALSE(storage.setnx("foo", "baz"));
    REQUIRE(storage.get("foo").value() == "bar");
}

TEST_CASE("rename moves a value to a new key", "[storage]") {
    auto path = temp_aof_path("rename");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    REQUIRE(storage.rename("foo", "baz"));
    REQUIRE_FALSE(storage.exists("foo"));
    REQUIRE(storage.get("baz").value() == "bar");
}

TEST_CASE("rename fails when source key is missing", "[storage]") {
    auto path = temp_aof_path("rename_missing");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.rename("nope", "dest"));
}

TEST_CASE("rename overwrites an existing destination key", "[storage]") {
    auto path = temp_aof_path("rename_overwrite");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    storage.set("baz", "old");
    REQUIRE(storage.rename("foo", "baz"));
    REQUIRE(storage.get("baz").value() == "bar");
    REQUIRE_FALSE(storage.exists("foo"));
}

TEST_CASE("expire sets a ttl on an existing key", "[storage]") {
    auto path = temp_aof_path("expire");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.expire("missing", 10));

    storage.set("foo", "bar");
    REQUIRE(storage.ttl("foo") == -1);
    REQUIRE(storage.expire("foo", 100));
    int t = storage.ttl("foo");
    REQUIRE(t > 0);
    REQUIRE(t <= 100);
}

TEST_CASE("expire with non-positive ttl deletes the key", "[storage]") {
    auto path = temp_aof_path("expire_delete");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    REQUIRE(storage.expire("foo", 0));
    REQUIRE_FALSE(storage.exists("foo"));
}

TEST_CASE("persist removes a key's ttl", "[storage]") {
    auto path = temp_aof_path("persist");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.persist("missing"));

    storage.set("foo", "bar");
    REQUIRE_FALSE(storage.persist("foo"));

    storage.set("foo", "bar", 100);
    REQUIRE(storage.persist("foo"));
    REQUIRE(storage.ttl("foo") == -1);
}

TEST_CASE("pexpire sets a millisecond ttl on an existing key", "[storage]") {
    auto path = temp_aof_path("pexpire");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.pexpire("missing", 10000));

    storage.set("foo", "bar");
    REQUIRE(storage.pttl("foo") == -1);
    REQUIRE(storage.pexpire("foo", 100000));
    long long t = storage.pttl("foo");
    REQUIRE(t > 0);
    REQUIRE(t <= 100000);
}

TEST_CASE("pexpire with non-positive ttl deletes the key", "[storage]") {
    auto path = temp_aof_path("pexpire_delete");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "bar");
    REQUIRE(storage.pexpire("foo", 0));
    REQUIRE_FALSE(storage.exists("foo"));
}

TEST_CASE("pttl reports -1 for no expiry and -2 for missing key", "[storage]") {
    auto path = temp_aof_path("pttl");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.pttl("missing") == -2);
    storage.set("persistent", "v");
    REQUIRE(storage.pttl("persistent") == -1);
}

TEST_CASE("keys matches glob patterns", "[storage]") {
    auto path = temp_aof_path("keys");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "1");
    storage.set("foobar", "2");
    storage.set("bar", "3");

    auto all = storage.keys("*");
    std::sort(all.begin(), all.end());
    REQUIRE(all == std::vector<std::string>{"bar", "foo", "foobar"});

    auto foo_star = storage.keys("foo*");
    std::sort(foo_star.begin(), foo_star.end());
    REQUIRE(foo_star == std::vector<std::string>{"foo", "foobar"});

    auto exact = storage.keys("bar");
    REQUIRE(exact == std::vector<std::string>{"bar"});

    auto q = storage.keys("fo?");
    REQUIRE(q == std::vector<std::string>{"foo"});

    auto none = storage.keys("nomatch*");
    REQUIRE(none.empty());
}

TEST_CASE("keys excludes expired entries", "[storage]") {
    auto path = temp_aof_path("keys_expired");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("persistent", "v");
    storage.set("ephemeral", "v", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto all = storage.keys("*");
    REQUIRE(all == std::vector<std::string>{"persistent"});
}

TEST_CASE("scan iterates all keys across multiple calls", "[storage]") {
    auto path = temp_aof_path("scan");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("a", "1");
    storage.set("b", "2");
    storage.set("c", "3");
    storage.set("d", "4");
    storage.set("e", "5");

    std::vector<std::string> collected;
    size_t cursor = 0;
    do {
        auto [next, batch] = storage.scan(cursor, "*", 2);
        collected.insert(collected.end(), batch.begin(), batch.end());
        cursor = next;
    } while (cursor != 0);

    std::sort(collected.begin(), collected.end());
    REQUIRE(collected == std::vector<std::string>{"a", "b", "c", "d", "e"});
}

TEST_CASE("scan respects match pattern", "[storage]") {
    auto path = temp_aof_path("scan_match");
    std::remove(path.c_str());
    Storage storage(path);

    storage.set("foo", "1");
    storage.set("foobar", "2");
    storage.set("bar", "3");

    auto [next, batch] = storage.scan(0, "foo*", 10);
    REQUIRE(next == 0);
    std::sort(batch.begin(), batch.end());
    REQUIRE(batch == std::vector<std::string>{"foo", "foobar"});
}

TEST_CASE("scan on empty storage returns cursor 0 and no keys", "[storage]") {
    auto path = temp_aof_path("scan_empty");
    std::remove(path.c_str());
    Storage storage(path);

    auto [next, batch] = storage.scan(0, "*", 10);
    REQUIRE(next == 0);
    REQUIRE(batch.empty());
}

TEST_CASE("hset creates a field and hget reads it back", "[storage]") {
    auto path = temp_aof_path("hset_hget");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hset("user", "name", "alice"));
    REQUIRE(storage.hget("user", "name").value() == "alice");
    REQUIRE_FALSE(storage.hget("user", "missing").has_value());
    REQUIRE_FALSE(storage.hget("nokey", "name").has_value());
}

TEST_CASE("hset returns false when updating an existing field", "[storage]") {
    auto path = temp_aof_path("hset_update");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hset("user", "name", "alice"));
    REQUIRE_FALSE(storage.hset("user", "name", "bob"));
    REQUIRE(storage.hget("user", "name").value() == "bob");
}

TEST_CASE("del removes a hash key", "[storage]") {
    auto path = temp_aof_path("del_hash");
    std::remove(path.c_str());
    Storage storage(path);

    storage.hset("user", "name", "alice");
    REQUIRE(storage.exists("user"));
    REQUIRE(storage.del("user"));
    REQUIRE_FALSE(storage.exists("user"));
    REQUIRE_FALSE(storage.hget("user", "name").has_value());
}

TEST_CASE("hdel removes fields and reports the removed count", "[storage]") {
    auto path = temp_aof_path("hdel");
    std::remove(path.c_str());
    Storage storage(path);

    storage.hset("user", "name", "alice");
    storage.hset("user", "age", "30");

    REQUIRE(storage.hdel("user", {"name", "missing"}) == 1);
    REQUIRE_FALSE(storage.hget("user", "name").has_value());
    REQUIRE(storage.hget("user", "age").value() == "30");
}

TEST_CASE("hdel removes the key entirely once all fields are gone", "[storage]") {
    auto path = temp_aof_path("hdel_empty");
    std::remove(path.c_str());
    Storage storage(path);

    storage.hset("user", "name", "alice");
    REQUIRE(storage.hdel("user", {"name"}) == 1);
    REQUIRE_FALSE(storage.exists("user"));
    REQUIRE(storage.hdel("user", {"name"}) == 0);
}

TEST_CASE("hgetall returns all field-value pairs", "[storage]") {
    auto path = temp_aof_path("hgetall");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hgetall("missing").empty());

    storage.hset("user", "name", "alice");
    storage.hset("user", "age", "30");
    auto pairs = storage.hgetall("user");
    std::sort(pairs.begin(), pairs.end());
    REQUIRE(pairs.size() == 2);
    REQUIRE(pairs[0] == std::make_pair(std::string("age"), std::string("30")));
    REQUIRE(pairs[1] == std::make_pair(std::string("name"), std::string("alice")));
}

TEST_CASE("hexists reports field presence", "[storage]") {
    auto path = temp_aof_path("hexists");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE_FALSE(storage.hexists("user", "name"));
    storage.hset("user", "name", "alice");
    REQUIRE(storage.hexists("user", "name"));
    REQUIRE_FALSE(storage.hexists("user", "age"));
}

TEST_CASE("hlen reports the number of fields", "[storage]") {
    auto path = temp_aof_path("hlen");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hlen("user") == 0);
    storage.hset("user", "name", "alice");
    storage.hset("user", "age", "30");
    REQUIRE(storage.hlen("user") == 2);
    storage.hdel("user", {"name"});
    REQUIRE(storage.hlen("user") == 1);
}

TEST_CASE("hkeys returns all field names", "[storage]") {
    auto path = temp_aof_path("hkeys");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hkeys("missing").empty());

    storage.hset("user", "name", "alice");
    storage.hset("user", "age", "30");
    auto fields = storage.hkeys("user");
    std::sort(fields.begin(), fields.end());
    REQUIRE(fields == std::vector<std::string>{"age", "name"});
}

TEST_CASE("hvals returns all field values", "[storage]") {
    auto path = temp_aof_path("hvals");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.hvals("missing").empty());

    storage.hset("user", "name", "alice");
    storage.hset("user", "age", "30");
    auto values = storage.hvals("user");
    std::sort(values.begin(), values.end());
    REQUIRE(values == std::vector<std::string>{"30", "alice"});
}

TEST_CASE("lpush prepends values so the last argument ends up at the head", "[storage]") {
    auto path = temp_aof_path("lpush");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.lpush("mylist", {"a", "b", "c"}) == 3);
    REQUIRE(storage.lrange("mylist", 0, -1) == std::vector<std::string>{"c", "b", "a"});
}

TEST_CASE("rpush appends values in argument order", "[storage]") {
    auto path = temp_aof_path("rpush");
    std::remove(path.c_str());
    Storage storage(path);

    REQUIRE(storage.rpush("mylist", {"a", "b", "c"}) == 3);
    REQUIRE(storage.lrange("mylist", 0, -1) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("lpush and rpush accumulate onto an existing list", "[storage]") {
    auto path = temp_aof_path("lpush_rpush_mix");
    std::remove(path.c_str());
    Storage storage(path);

    storage.rpush("mylist", {"b", "c"});
    REQUIRE(storage.lpush("mylist", {"a"}) == 3);
    REQUIRE(storage.rpush("mylist", {"d"}) == 4);
    REQUIRE(storage.lrange("mylist", 0, -1) == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("del removes a list key", "[storage]") {
    auto path = temp_aof_path("del_list");
    std::remove(path.c_str());
    Storage storage(path);

    storage.rpush("mylist", {"a"});
    REQUIRE(storage.exists("mylist"));
    REQUIRE(storage.del("mylist"));
    REQUIRE_FALSE(storage.exists("mylist"));
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
