#pragma once

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <optional>
#include <chrono>
#include <fstream>
#include <vector>

namespace litekv {

struct Entry {
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
};

class Storage {
public:
    explicit Storage(const std::string& aof_path = "litekv.aof",
                     size_t max_keys = 10000);

    void set(const std::string& key, const std::string& value, int ttl_seconds = -1);
    std::optional<std::string> get(const std::string& key);
    std::optional<std::string> getset(const std::string& key, const std::string& value);
    bool setnx(const std::string& key, const std::string& value);
    bool rename(const std::string& key, const std::string& newkey);
    bool del(const std::string& key);
    std::optional<long long> incrby(const std::string& key, long long delta);
    long long append(const std::string& key, const std::string& value);
    long long strlen(const std::string& key);
    void mset(const std::vector<std::pair<std::string, std::string>>& pairs);
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);
    bool exists(const std::string& key);
    int ttl(const std::string& key);
    bool expire(const std::string& key, int ttl_seconds);
    bool persist(const std::string& key);
    bool pexpire(const std::string& key, long long ttl_ms);
    long long pttl(const std::string& key);
    std::vector<std::string> keys(const std::string& pattern);
    std::pair<size_t, std::vector<std::string>> scan(size_t cursor, const std::string& pattern,
                                                       size_t count);
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    std::optional<std::string> hget(const std::string& key, const std::string& field);
    long long hdel(const std::string& key, const std::vector<std::string>& fields);
    std::vector<std::pair<std::string, std::string>> hgetall(const std::string& key);
    bool hexists(const std::string& key, const std::string& field);
    long long hlen(const std::string& key);
    void flush();
    void load_aof();
    size_t size() const;
    std::vector<std::vector<std::string>> dump_commands();

private:
    // LRU: list stores keys in order (front = most recent)
    std::list<std::string> lru_list_;
    // map: key → {entry, iterator to lru_list_}
    std::unordered_map<std::string,
        std::pair<Entry, std::list<std::string>::iterator>> data_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hashes_;

    std::mutex mutex_;
    std::string aof_path_;
    std::ofstream aof_file_;
    size_t max_keys_;

    bool is_expired(const Entry& entry) const;
    void append_aof(const std::string& line);
    void evict_lru();
    void touch(const std::string& key);
    static bool glob_match(const std::string& pattern, const std::string& str);
};

} // namespace litekv