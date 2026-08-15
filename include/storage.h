#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>
#include <chrono>
#include <fstream>
#include <vector>
#include <cstdint>

namespace litekv {

struct Entry {
    std::string value;
    std::optional<std::chrono::steady_clock::time_point> expires_at;
    uint64_t freq = 1;
};

enum class AofFsyncPolicy { ALWAYS, EVERYSEC, NO };
enum class EvictionPolicy { LRU, LFU };

class Storage {
public:
    explicit Storage(const std::string& aof_path = "litekv.aof",
                     size_t max_keys = 10000);
    ~Storage();
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    void set(const std::string& key, const std::string& value, int ttl_seconds = -1);
    std::optional<std::string> get(const std::string& key);
    std::optional<std::string> getset(const std::string& key, const std::string& value);
    bool setnx(const std::string& key, const std::string& value);
    bool rename(const std::string& key, const std::string& newkey);
    bool copy(const std::string& key, const std::string& newkey, bool replace);
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
    std::optional<std::string> randomkey();
    std::optional<std::string> object_encoding(const std::string& key);
    std::pair<size_t, std::vector<std::string>> scan(size_t cursor, const std::string& pattern,
                                                       size_t count);
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    std::optional<std::string> hget(const std::string& key, const std::string& field);
    long long hdel(const std::string& key, const std::vector<std::string>& fields);
    std::vector<std::pair<std::string, std::string>> hgetall(const std::string& key);
    bool hexists(const std::string& key, const std::string& field);
    long long hlen(const std::string& key);
    std::vector<std::string> hkeys(const std::string& key);
    std::vector<std::string> hvals(const std::string& key);
    long long lpush(const std::string& key, const std::vector<std::string>& values);
    long long rpush(const std::string& key, const std::vector<std::string>& values);
    std::optional<std::string> lpop(const std::string& key);
    std::optional<std::string> rpop(const std::string& key);
    std::vector<std::string> lrange(const std::string& key, long long start, long long stop);
    long long llen(const std::string& key);
    long long sadd(const std::string& key, const std::vector<std::string>& members);
    long long srem(const std::string& key, const std::vector<std::string>& members);
    std::vector<std::string> smembers(const std::string& key);
    bool sismember(const std::string& key, const std::string& member);
    long long scard(const std::string& key);
    long long zadd(const std::string& key, const std::vector<std::pair<double, std::string>>& members);
    std::optional<double> zscore(const std::string& key, const std::string& member);
    std::vector<std::string> zrange(const std::string& key, long long start, long long stop);
    void flush();
    void load_aof();
    bool save();
    bool rewrite_aof();
    void set_aof_fsync_policy(AofFsyncPolicy policy);
    void set_eviction_policy(EvictionPolicy policy);
    size_t size() const;
    std::vector<std::vector<std::string>> dump_commands();
    uint64_t version();
    static bool glob_match(const std::string& pattern, const std::string& str);

private:
    // LRU: list stores keys in order (front = most recent)
    std::list<std::string> lru_list_;
    // map: key → {entry, iterator to lru_list_}
    std::unordered_map<std::string,
        std::pair<Entry, std::list<std::string>::iterator>> data_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hashes_;
    std::unordered_map<std::string, std::deque<std::string>> lists_;
    std::unordered_map<std::string, std::unordered_set<std::string>> sets_;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> zsets_;

    std::mutex mutex_;
    std::string aof_path_;
    std::ofstream aof_file_;
    size_t max_keys_;
    uint64_t version_ = 0;

    AofFsyncPolicy fsync_policy_ = AofFsyncPolicy::EVERYSEC;
    EvictionPolicy eviction_policy_ = EvictionPolicy::LRU;
    bool aof_dirty_ = false;
    std::atomic<bool> fsync_thread_running_{false};
    std::thread fsync_thread_;
    std::mutex fsync_cv_mutex_;
    std::condition_variable fsync_cv_;

    bool is_expired(const Entry& entry) const;
    void append_aof(const std::string& line);
    void evict();
    void evict_lru();
    void evict_lfu();
    void touch(const std::string& key);
    std::vector<std::vector<std::string>> dump_commands_locked();
    void fsync_aof_locked();
    void start_fsync_thread();
    void stop_fsync_thread();
};

} // namespace litekv