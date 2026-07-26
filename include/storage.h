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
    bool del(const std::string& key);
    std::optional<long long> incrby(const std::string& key, long long delta);
    long long append(const std::string& key, const std::string& value);
    bool exists(const std::string& key);
    int ttl(const std::string& key);
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

    std::mutex mutex_;
    std::string aof_path_;
    std::ofstream aof_file_;
    size_t max_keys_;

    bool is_expired(const Entry& entry) const;
    void append_aof(const std::string& line);
    void evict_lru();
    void touch(const std::string& key);
};

} // namespace litekv