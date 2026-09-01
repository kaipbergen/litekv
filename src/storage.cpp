#include "storage.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace litekv {

Storage::Storage(const std::string& aof_path, size_t max_keys)
    : aof_path_(aof_path), max_keys_(max_keys) {
    aof_file_.open(aof_path_, std::ios::app);
    if (!aof_file_.is_open()) {
        std::cerr << "Warning: Could not open AOF file: " << aof_path_ << std::endl;
    }
    start_fsync_thread();
}

Storage::~Storage() {
    stop_fsync_thread();
}

void Storage::start_fsync_thread() {
    fsync_thread_running_ = true;
    fsync_thread_ = std::thread([this]() {
        std::unique_lock<std::mutex> lk(fsync_cv_mutex_);
        while (fsync_thread_running_) {
            fsync_cv_.wait_for(lk, std::chrono::seconds(1));
            if (!fsync_thread_running_) break;
            lk.unlock();
            {
                std::lock_guard<std::mutex> dlock(mutex_);
                if (fsync_policy_ == AofFsyncPolicy::EVERYSEC && aof_dirty_) {
                    fsync_aof_locked();
                    aof_dirty_ = false;
                }
            }
            lk.lock();
        }
    });
}

void Storage::stop_fsync_thread() {
    {
        std::lock_guard<std::mutex> lk(fsync_cv_mutex_);
        fsync_thread_running_ = false;
    }
    fsync_cv_.notify_all();
    if (fsync_thread_.joinable()) fsync_thread_.join();
}

void Storage::fsync_aof_locked() {
    if (!aof_file_.is_open()) return;
    aof_file_.flush();
    int fd = ::open(aof_path_.c_str(), O_WRONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

void Storage::set_aof_fsync_policy(AofFsyncPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    fsync_policy_ = policy;
}

void Storage::set_eviction_policy(EvictionPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    eviction_policy_ = policy;
}

bool Storage::is_expired(const Entry& entry) const {
    if (!entry.expires_at.has_value()) return false;
    return std::chrono::steady_clock::now() > entry.expires_at.value();
}

static const std::string kAofChecksumMarker = " ;;chk=";

static uint32_t aof_crc32(const std::string& data) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void Storage::append_aof(const std::string& line) {
    version_++;
    if (aof_file_.is_open()) {
        char checksum_hex[9];
        snprintf(checksum_hex, sizeof(checksum_hex), "%08x", aof_crc32(line));
        aof_file_ << line << kAofChecksumMarker << checksum_hex << "\n";
        aof_file_.flush();
        if (fsync_policy_ == AofFsyncPolicy::ALWAYS) {
            fsync_aof_locked();
        } else if (fsync_policy_ == AofFsyncPolicy::EVERYSEC) {
            aof_dirty_ = true;
        }
    }
}

uint64_t Storage::version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

void Storage::touch(const std::string& key) {
    auto it = data_.find(key);
    if (it != data_.end()) {
        it->second.first.freq++;
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    }
}

void Storage::evict() {
    if (eviction_policy_ == EvictionPolicy::LFU) {
        evict_lfu();
    } else if (eviction_policy_ == EvictionPolicy::RANDOM) {
        evict_random();
    } else {
        evict_lru();
    }
}

void Storage::evict_lru() {
    while (data_.size() >= max_keys_) {
        const std::string& lru_key = lru_list_.back();
        data_.erase(lru_key);
        lru_list_.pop_back();
        std::cout << "LRU evicted: " << lru_key << std::endl;
    }
}

void Storage::evict_lfu() {
    while (data_.size() >= max_keys_) {
        auto min_it = data_.begin();
        for (auto it = data_.begin(); it != data_.end(); ++it) {
            if (it->second.first.freq < min_it->second.first.freq) min_it = it;
        }
        std::string victim = min_it->first;
        lru_list_.erase(min_it->second.second);
        data_.erase(min_it);
        std::cout << "LFU evicted: " << victim << std::endl;
    }
}

void Storage::evict_random() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    while (data_.size() >= max_keys_) {
        size_t idx = rng() % data_.size();
        auto it = data_.begin();
        std::advance(it, idx);
        std::string victim = it->first;
        lru_list_.erase(it->second.second);
        data_.erase(it);
        std::cout << "Randomly evicted: " << victim << std::endl;
    }
}

void Storage::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove existing entry
    auto it = data_.find(key);
    if (it != data_.end()) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
    }

    evict();

    Entry entry;
    entry.value = value;
    if (ttl_seconds > 0) {
        entry.expires_at = std::chrono::steady_clock::now() +
                           std::chrono::seconds(ttl_seconds);
        append_aof("SET " + key + " " + value + " EX " + std::to_string(ttl_seconds));
    } else {
        append_aof("SET " + key + " " + value);
    }

    lru_list_.push_front(key);
    data_[key] = {std::move(entry), lru_list_.begin()};
}

std::optional<std::string> Storage::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return std::nullopt;
    }
    touch(key);
    return it->second.first.value;
}

std::optional<std::string> Storage::getset(const std::string& key, const std::string& value) {
    auto old_value = get(key);
    set(key, value);
    return old_value;
}

std::optional<std::string> Storage::getdel(const std::string& key) {
    auto value = get(key);
    if (value.has_value()) del(key);
    return value;
}

bool Storage::setnx(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }
    if (it != data_.end()) return false;

    evict();
    Entry entry;
    entry.value = value;
    append_aof("SET " + key + " " + value);
    lru_list_.push_front(key);
    data_[key] = {std::move(entry), lru_list_.begin()};
    return true;
}

bool Storage::rename(const std::string& key, const std::string& newkey) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }
    if (it == data_.end()) return false;

    Entry entry = it->second.first;
    lru_list_.erase(it->second.second);
    data_.erase(it);

    if (key != newkey) {
        auto newit = data_.find(newkey);
        if (newit != data_.end()) {
            lru_list_.erase(newit->second.second);
            data_.erase(newit);
        }
    }

    if (entry.expires_at.has_value()) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            entry.expires_at.value() - std::chrono::steady_clock::now()).count();
        append_aof("SET " + newkey + " " + entry.value + " EX " + std::to_string(remaining));
    } else {
        append_aof("SET " + newkey + " " + entry.value);
    }
    if (key != newkey) append_aof("DEL " + key);

    lru_list_.push_front(newkey);
    data_[newkey] = {std::move(entry), lru_list_.begin()};
    return true;
}

std::optional<long long> Storage::incrby(const std::string& key, long long delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }

    long long current = 0;
    if (it != data_.end()) {
        const std::string& val = it->second.first.value;
        try {
            size_t pos;
            current = std::stoll(val, &pos);
            if (pos != val.size()) return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    long long updated = current + delta;
    std::string value_str = std::to_string(updated);

    if (it != data_.end()) {
        it->second.first.value = value_str;
        it->second.first.freq++;
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    } else {
        evict();
        Entry entry;
        entry.value = value_str;
        lru_list_.push_front(key);
        data_[key] = {std::move(entry), lru_list_.begin()};
    }

    append_aof("SET " + key + " " + value_str);
    return updated;
}

long long Storage::append(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }

    std::string new_value;
    if (it != data_.end()) {
        new_value = it->second.first.value + value;
        it->second.first.value = new_value;
        it->second.first.freq++;
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    } else {
        new_value = value;
        evict();
        Entry entry;
        entry.value = new_value;
        lru_list_.push_front(key);
        data_[key] = {std::move(entry), lru_list_.begin()};
    }

    append_aof("SET " + key + " " + new_value);
    return static_cast<long long>(new_value.size());
}

long long Storage::strlen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return 0;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return 0;
    }
    return static_cast<long long>(it->second.first.value.size());
}

void Storage::mset(const std::vector<std::pair<std::string, std::string>>& pairs) {
    for (const auto& [key, value] : pairs) {
        set(key, value);
    }
}

std::vector<std::optional<std::string>> Storage::mget(const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> results;
    results.reserve(keys.size());
    for (const auto& key : keys) {
        results.push_back(get(key));
    }
    return results;
}

bool Storage::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool deleted = false;
    auto it = data_.find(key);
    if (it != data_.end()) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        deleted = true;
    }
    if (hashes_.erase(key) > 0) deleted = true;
    if (lists_.erase(key) > 0) deleted = true;
    if (sets_.erase(key) > 0) deleted = true;
    if (deleted) append_aof("DEL " + key);
    return deleted;
}

bool Storage::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it != data_.end()) {
        if (is_expired(it->second.first)) {
            lru_list_.erase(it->second.second);
            data_.erase(it);
        } else {
            return true;
        }
    }
    if (hashes_.find(key) != hashes_.end()) return true;
    if (lists_.find(key) != lists_.end()) return true;
    return sets_.find(key) != sets_.end();
}

long long Storage::touch(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long count = 0;
    for (const auto& key : keys) {
        auto it = data_.find(key);
        if (it != data_.end()) {
            if (is_expired(it->second.first)) {
                lru_list_.erase(it->second.second);
                data_.erase(it);
            } else {
                touch(key);
                count++;
                continue;
            }
        }
        if (hashes_.find(key) != hashes_.end()) { count++; continue; }
        if (lists_.find(key) != lists_.end()) { count++; continue; }
        if (sets_.find(key) != sets_.end()) { count++; continue; }
    }
    return count;
}

int Storage::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return -2;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return -2;
    }
    if (!it->second.first.expires_at.has_value()) return -1;
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.first.expires_at.value() - std::chrono::steady_clock::now()
    ).count();
    return static_cast<int>(remaining);
}

bool Storage::expire(const std::string& key, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return false;
    }
    if (ttl_seconds <= 0) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        append_aof("DEL " + key);
        return true;
    }
    it->second.first.expires_at = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(ttl_seconds);
    append_aof("SET " + key + " " + it->second.first.value + " EX " + std::to_string(ttl_seconds));
    return true;
}

bool Storage::persist(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return false;
    }
    if (!it->second.first.expires_at.has_value()) return false;
    it->second.first.expires_at.reset();
    append_aof("SET " + key + " " + it->second.first.value);
    return true;
}

bool Storage::pexpire(const std::string& key, long long ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return false;
    }
    if (ttl_ms <= 0) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        append_aof("DEL " + key);
        return true;
    }
    it->second.first.expires_at = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(ttl_ms);
    long long ttl_seconds = (ttl_ms + 999) / 1000;
    append_aof("SET " + key + " " + it->second.first.value + " EX " + std::to_string(ttl_seconds));
    return true;
}

long long Storage::pttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return -2;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return -2;
    }
    if (!it->second.first.expires_at.has_value()) return -1;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        it->second.first.expires_at.value() - std::chrono::steady_clock::now()
    ).count();
    return remaining;
}

bool Storage::glob_match(const std::string& pattern, const std::string& str) {
    size_t p = 0, s = 0;
    size_t star_p = std::string::npos, star_s = 0;
    size_t plen = pattern.size(), slen = str.size();

    while (s < slen) {
        if (p < plen && pattern[p] == '\\' && p + 1 < plen) {
            if (str[s] == pattern[p + 1]) { p += 2; s++; continue; }
        } else if (p < plen && pattern[p] == '[') {
            size_t close = p + 1;
            bool negate = close < plen && (pattern[close] == '^');
            if (negate) close++;
            size_t class_start = close;
            bool matched = false;
            bool first = true;
            while (close < plen && (pattern[close] != ']' || first)) {
                first = false;
                if (pattern[close] == '-' && close + 1 < plen && pattern[close + 1] != ']' &&
                    close > class_start) {
                    char lo = pattern[close - 1];
                    char hi = pattern[close + 1];
                    if (str[s] >= std::min(lo, hi) && str[s] <= std::max(lo, hi)) matched = true;
                    close += 2;
                } else {
                    if (pattern[close] == str[s]) matched = true;
                    close++;
                }
            }
            if (close < plen) close++; // consume ']'
            if (matched != negate) { p = close; s++; continue; }
        } else if (p < plen && pattern[p] == '?') {
            p++; s++; continue;
        } else if (p < plen && pattern[p] == '*') {
            star_p = p; star_s = s; p++; continue;
        } else if (p < plen && pattern[p] == str[s]) {
            p++; s++; continue;
        }

        if (star_p != std::string::npos) {
            p = star_p + 1;
            star_s++;
            s = star_s;
        } else {
            return false;
        }
    }
    while (p < plen && pattern[p] == '*') p++;
    return p == plen;
}

std::vector<std::string> Storage::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [key, pair] : data_) {
        const Entry& entry = pair.first;
        if (entry.expires_at.has_value() && now > entry.expires_at.value()) continue;
        if (glob_match(pattern, key)) result.push_back(key);
    }
    return result;
}

std::optional<std::string> Storage::randomkey() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> live_keys;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [key, pair] : data_) {
        const Entry& entry = pair.first;
        if (entry.expires_at.has_value() && now > entry.expires_at.value()) continue;
        live_keys.push_back(key);
    }
    for (const auto& [key, fields] : hashes_) live_keys.push_back(key);
    for (const auto& [key, lst] : lists_) live_keys.push_back(key);
    for (const auto& [key, set] : sets_) live_keys.push_back(key);
    for (const auto& [key, zset] : zsets_) live_keys.push_back(key);

    if (live_keys.empty()) return std::nullopt;

    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, live_keys.size() - 1);
    return live_keys[dist(rng)];
}

std::optional<std::string> Storage::object_encoding(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hashes_.count(key)) return std::string("hashtable");
    if (lists_.count(key)) return std::string("quicklist");

    auto sit = sets_.find(key);
    if (sit != sets_.end()) {
        bool all_int = true;
        for (const auto& m : sit->second) {
            try {
                size_t pos;
                std::stoll(m, &pos);
                if (pos != m.size()) { all_int = false; break; }
            } catch (...) { all_int = false; break; }
        }
        return all_int ? std::string("intset") : std::string("hashtable");
    }

    if (zsets_.count(key)) return std::string("skiplist");

    auto dit = data_.find(key);
    if (dit != data_.end() && !is_expired(dit->second.first)) {
        const std::string& val = dit->second.first.value;
        try {
            size_t pos;
            std::stoll(val, &pos);
            if (!val.empty() && pos == val.size()) return std::string("int");
        } catch (...) {}
        return val.size() <= 44 ? std::string("embstr") : std::string("raw");
    }

    return std::nullopt;
}

std::pair<size_t, std::vector<std::string>> Storage::scan(size_t cursor, const std::string& pattern,
                                                            size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> all_keys;
    all_keys.reserve(data_.size());
    auto now = std::chrono::steady_clock::now();
    for (const auto& [key, pair] : data_) {
        const Entry& entry = pair.first;
        if (entry.expires_at.has_value() && now > entry.expires_at.value()) continue;
        all_keys.push_back(key);
    }
    std::sort(all_keys.begin(), all_keys.end());

    std::vector<std::string> result;
    if (count == 0) count = 10;
    size_t i = cursor;
    for (; i < all_keys.size() && result.size() < count; i++) {
        if (glob_match(pattern, all_keys[i])) result.push_back(all_keys[i]);
    }
    size_t next_cursor = (i >= all_keys.size()) ? 0 : i;
    return {next_cursor, result};
}

bool Storage::hset(const std::string& key, const std::string& field, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& fields = hashes_[key];
    bool is_new = fields.find(field) == fields.end();
    fields[field] = value;
    append_aof("HSET " + key + " " + field + " " + value);
    return is_new;
}

std::optional<std::string> Storage::hget(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return std::nullopt;
    auto fit = it->second.find(field);
    if (fit == it->second.end()) return std::nullopt;
    return fit->second;
}

long long Storage::hdel(const std::string& key, const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return 0;
    long long removed = 0;
    for (const auto& field : fields) {
        if (it->second.erase(field) > 0) {
            removed++;
            append_aof("HDEL " + key + " " + field);
        }
    }
    if (it->second.empty()) hashes_.erase(it);
    return removed;
}

std::vector<std::pair<std::string, std::string>> Storage::hgetall(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, std::string>> result;
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return result;
    result.reserve(it->second.size());
    for (const auto& [field, value] : it->second) {
        result.emplace_back(field, value);
    }
    return result;
}

bool Storage::hexists(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return false;
    return it->second.find(field) != it->second.end();
}

long long Storage::hlen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return 0;
    return static_cast<long long>(it->second.size());
}

std::vector<std::string> Storage::hkeys(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return result;
    result.reserve(it->second.size());
    for (const auto& [field, value] : it->second) {
        result.push_back(field);
    }
    return result;
}

std::vector<std::string> Storage::hvals(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = hashes_.find(key);
    if (it == hashes_.end()) return result;
    result.reserve(it->second.size());
    for (const auto& [field, value] : it->second) {
        result.push_back(value);
    }
    return result;
}

long long Storage::lpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& lst = lists_[key];
    for (const auto& v : values) lst.push_front(v);

    std::string line = "LPUSH " + key;
    for (const auto& v : values) line += " " + v;
    append_aof(line);

    return static_cast<long long>(lst.size());
}

long long Storage::rpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& lst = lists_[key];
    for (const auto& v : values) lst.push_back(v);

    std::string line = "RPUSH " + key;
    for (const auto& v : values) line += " " + v;
    append_aof(line);

    return static_cast<long long>(lst.size());
}

std::optional<std::string> Storage::lpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lists_.find(key);
    if (it == lists_.end() || it->second.empty()) return std::nullopt;
    std::string val = it->second.front();
    it->second.pop_front();
    if (it->second.empty()) lists_.erase(it);
    append_aof("LPOP " + key);
    return val;
}

std::optional<std::string> Storage::rpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lists_.find(key);
    if (it == lists_.end() || it->second.empty()) return std::nullopt;
    std::string val = it->second.back();
    it->second.pop_back();
    if (it->second.empty()) lists_.erase(it);
    append_aof("RPOP " + key);
    return val;
}

std::vector<std::string> Storage::lrange(const std::string& key, long long start, long long stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = lists_.find(key);
    if (it == lists_.end()) return result;

    const auto& lst = it->second;
    long long len = static_cast<long long>(lst.size());
    if (len == 0) return result;

    if (start < 0) start = std::max(len + start, 0LL);
    if (stop < 0) stop = len + stop;
    if (stop >= len) stop = len - 1;
    if (start > stop || start >= len) return result;

    result.reserve(static_cast<size_t>(stop - start + 1));
    auto lit = lst.begin();
    std::advance(lit, start);
    for (long long i = start; i <= stop; i++, ++lit) {
        result.push_back(*lit);
    }
    return result;
}

long long Storage::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lists_.find(key);
    if (it == lists_.end()) return 0;
    return static_cast<long long>(it->second.size());
}

long long Storage::sadd(const std::string& key, const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& set = sets_[key];
    long long added = 0;
    std::string line = "SADD " + key;
    for (const auto& m : members) {
        if (set.insert(m).second) {
            added++;
            line += " " + m;
        }
    }
    if (added > 0) append_aof(line);
    return added;
}

long long Storage::srem(const std::string& key, const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return 0;

    long long removed = 0;
    std::string line = "SREM " + key;
    for (const auto& m : members) {
        if (it->second.erase(m) > 0) {
            removed++;
            line += " " + m;
        }
    }
    if (it->second.empty()) sets_.erase(it);
    if (removed > 0) append_aof(line);
    return removed;
}

std::vector<std::string> Storage::smembers(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = sets_.find(key);
    if (it == sets_.end()) return result;
    result.reserve(it->second.size());
    for (const auto& m : it->second) result.push_back(m);
    return result;
}

bool Storage::sismember(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return false;
    return it->second.count(member) > 0;
}

long long Storage::scard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sets_.find(key);
    if (it == sets_.end()) return 0;
    return static_cast<long long>(it->second.size());
}

static std::string format_double(double value) {
    if (std::isfinite(value) && value == static_cast<double>(static_cast<long long>(value)) &&
        std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", value);
    return std::string(buf);
}

std::optional<std::string> Storage::incrbyfloat(const std::string& key, double delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }

    double current = 0.0;
    if (it != data_.end()) {
        const std::string& val = it->second.first.value;
        try {
            size_t pos;
            current = std::stod(val, &pos);
            if (pos != val.size()) return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    double updated = current + delta;
    if (!std::isfinite(updated)) return std::nullopt;
    std::string value_str = format_double(updated);

    if (it != data_.end()) {
        it->second.first.value = value_str;
        it->second.first.freq++;
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    } else {
        evict();
        Entry entry;
        entry.value = value_str;
        lru_list_.push_front(key);
        data_[key] = {std::move(entry), lru_list_.begin()};
    }

    append_aof("SET " + key + " " + value_str);
    return value_str;
}

long long Storage::zadd(const std::string& key, const std::vector<std::pair<double, std::string>>& members) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& zset = zsets_[key];
    long long added = 0;
    std::string line = "ZADD " + key;
    for (const auto& [score, member] : members) {
        if (zset.find(member) == zset.end()) added++;
        zset[member] = score;
        line += " " + format_double(score) + " " + member;
    }
    append_aof(line);
    return added;
}

std::optional<double> Storage::zscore(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = zsets_.find(key);
    if (it == zsets_.end()) return std::nullopt;
    auto mit = it->second.find(member);
    if (mit == it->second.end()) return std::nullopt;
    return mit->second;
}

std::vector<std::string> Storage::zrange(const std::string& key, long long start, long long stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    auto it = zsets_.find(key);
    if (it == zsets_.end()) return result;

    std::vector<std::pair<double, std::string>> sorted;
    sorted.reserve(it->second.size());
    for (const auto& [member, score] : it->second) sorted.emplace_back(score, member);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    long long len = static_cast<long long>(sorted.size());
    if (len == 0) return result;
    if (start < 0) start = std::max(len + start, 0LL);
    if (stop < 0) stop = len + stop;
    if (stop >= len) stop = len - 1;
    if (start > stop || start >= len) return result;

    result.reserve(static_cast<size_t>(stop - start + 1));
    for (long long i = start; i <= stop; i++) result.push_back(sorted[static_cast<size_t>(i)].second);
    return result;
}

bool Storage::copy(const std::string& src, const std::string& dst, bool replace) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (src == dst) return false;

    auto sit = data_.find(src);
    bool src_is_string = sit != data_.end() && !is_expired(sit->second.first);
    Entry src_entry;
    if (src_is_string) src_entry = sit->second.first;
    bool src_is_hash = !src_is_string && hashes_.count(src) > 0;
    bool src_is_list = !src_is_string && !src_is_hash && lists_.count(src) > 0;
    bool src_is_set = !src_is_string && !src_is_hash && !src_is_list && sets_.count(src) > 0;
    bool src_is_zset = !src_is_string && !src_is_hash && !src_is_list && !src_is_set && zsets_.count(src) > 0;
    if (!src_is_string && !src_is_hash && !src_is_list && !src_is_set && !src_is_zset) return false;

    bool dst_exists = false;
    auto ddata = data_.find(dst);
    if (ddata != data_.end() && !is_expired(ddata->second.first)) dst_exists = true;
    if (hashes_.count(dst)) dst_exists = true;
    if (lists_.count(dst)) dst_exists = true;
    if (sets_.count(dst)) dst_exists = true;
    if (zsets_.count(dst)) dst_exists = true;
    if (dst_exists && !replace) return false;

    if (dst_exists) {
        auto dit = data_.find(dst);
        if (dit != data_.end()) {
            lru_list_.erase(dit->second.second);
            data_.erase(dit);
        }
        hashes_.erase(dst);
        lists_.erase(dst);
        sets_.erase(dst);
        zsets_.erase(dst);
        append_aof("DEL " + dst);
    }

    if (src_is_string) {
        evict();
        lru_list_.push_front(dst);
        data_[dst] = {src_entry, lru_list_.begin()};
        if (src_entry.expires_at.has_value()) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                src_entry.expires_at.value() - std::chrono::steady_clock::now()).count();
            append_aof("SET " + dst + " " + src_entry.value + " EX " + std::to_string(remaining));
        } else {
            append_aof("SET " + dst + " " + src_entry.value);
        }
    } else if (src_is_hash) {
        hashes_[dst] = hashes_[src];
        for (const auto& [f, v] : hashes_[dst]) append_aof("HSET " + dst + " " + f + " " + v);
    } else if (src_is_list) {
        lists_[dst] = lists_[src];
        std::string line = "RPUSH " + dst;
        for (const auto& v : lists_[dst]) line += " " + v;
        append_aof(line);
    } else if (src_is_set) {
        sets_[dst] = sets_[src];
        std::string line = "SADD " + dst;
        for (const auto& m : sets_[dst]) line += " " + m;
        append_aof(line);
    } else if (src_is_zset) {
        zsets_[dst] = zsets_[src];
        std::string line = "ZADD " + dst;
        for (const auto& [m, sc] : zsets_[dst]) line += " " + format_double(sc) + " " + m;
        append_aof(line);
    }
    return true;
}

void Storage::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    lru_list_.clear();
    hashes_.clear();
    lists_.clear();
    sets_.clear();
    zsets_.clear();
    append_aof("FLUSHALL");
}

size_t Storage::size() const {
    return data_.size();
}

std::vector<std::vector<std::string>> Storage::dump_commands() {
    std::lock_guard<std::mutex> lock(mutex_);
    return dump_commands_locked();
}

std::vector<std::vector<std::string>> Storage::dump_commands_locked() {
    std::vector<std::vector<std::string>> commands;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [key, pair] : data_) {
        const Entry& entry = pair.first;
        if (entry.expires_at.has_value() && now > entry.expires_at.value()) continue;

        std::vector<std::string> cmd = {"SET", key, entry.value};
        if (entry.expires_at.has_value()) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                entry.expires_at.value() - now).count();
            if (remaining > 0) {
                cmd.push_back("EX");
                cmd.push_back(std::to_string(remaining));
            }
        }
        commands.push_back(std::move(cmd));
    }
    for (const auto& [key, fields] : hashes_) {
        for (const auto& [field, value] : fields) {
            commands.push_back({"HSET", key, field, value});
        }
    }
    for (const auto& [key, lst] : lists_) {
        std::vector<std::string> cmd = {"RPUSH", key};
        for (const auto& v : lst) cmd.push_back(v);
        commands.push_back(std::move(cmd));
    }
    for (const auto& [key, set] : sets_) {
        std::vector<std::string> cmd = {"SADD", key};
        for (const auto& m : set) cmd.push_back(m);
        commands.push_back(std::move(cmd));
    }
    for (const auto& [key, zset] : zsets_) {
        std::vector<std::string> cmd = {"ZADD", key};
        for (const auto& [member, score] : zset) {
            cmd.push_back(format_double(score));
            cmd.push_back(member);
        }
        commands.push_back(std::move(cmd));
    }
    return commands;
}

bool Storage::save() {
    std::string tmp_path = aof_path_ + ".rdb.tmp";
    std::string final_path = aof_path_ + ".rdb";
    auto commands = dump_commands();

    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& cmd : commands) {
        for (size_t i = 0; i < cmd.size(); i++) {
            if (i > 0) out << ' ';
            out << cmd[i];
        }
        out << '\n';
    }
    out.close();
    if (out.fail()) return false;

    return std::rename(tmp_path.c_str(), final_path.c_str()) == 0;
}

bool Storage::rewrite_aof() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string tmp_path = aof_path_ + ".rewrite.tmp";
    auto commands = dump_commands_locked();

    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& cmd : commands) {
        for (size_t i = 0; i < cmd.size(); i++) {
            if (i > 0) out << ' ';
            out << cmd[i];
        }
        out << '\n';
    }
    out.close();
    if (out.fail()) return false;

    if (std::rename(tmp_path.c_str(), aof_path_.c_str()) != 0) return false;

    if (aof_file_.is_open()) aof_file_.close();
    aof_file_.open(aof_path_, std::ios::app);
    return aof_file_.is_open();
}

void Storage::load_aof() {
    std::string rdb_path = aof_path_ + ".rdb";
    struct stat aof_stat{};
    struct stat rdb_stat{};
    bool aof_exists = ::stat(aof_path_.c_str(), &aof_stat) == 0;
    bool rdb_exists = ::stat(rdb_path.c_str(), &rdb_stat) == 0;

    if (rdb_exists && (!aof_exists || rdb_stat.st_mtime > aof_stat.st_mtime)) {
        std::cout << "RDB snapshot is newer than AOF, loading from " << rdb_path << std::endl;
        load_from_path(rdb_path);
        return;
    }
    load_from_path(aof_path_);
}

void Storage::load_from_path(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    int loaded = 0;
    int corrupted = 0;
    int truncated = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        auto marker_pos = line.rfind(kAofChecksumMarker);
        if (marker_pos != std::string::npos) {
            std::string checksum_hex = line.substr(marker_pos + kAofChecksumMarker.size());
            std::string content = line.substr(0, marker_pos);
            bool valid_hex = checksum_hex.size() == 8 &&
                checksum_hex.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            uint32_t expected = 0;
            if (valid_hex) {
                try {
                    expected = static_cast<uint32_t>(std::stoul(checksum_hex, nullptr, 16));
                } catch (...) {
                    valid_hex = false;
                }
            }
            if (!valid_hex || aof_crc32(content) != expected) {
                std::cerr << "AOF: skipping corrupted entry (checksum mismatch)" << std::endl;
                corrupted++;
                continue;
            }
            line = content;
        }

        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        try {
        if (cmd == "SET") {
            std::string key, value, ex_flag;
            ss >> key >> value;
            Entry entry;
            entry.value = value;
            if (ss >> ex_flag && ex_flag == "EX") {
                int ttl;
                ss >> ttl;
                entry.expires_at = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(ttl);
            }
            lru_list_.push_front(key);
            data_[key] = {std::move(entry), lru_list_.begin()};
            loaded++;
        } else if (cmd == "DEL") {
            std::string key;
            ss >> key;
            auto it = data_.find(key);
            if (it != data_.end()) {
                lru_list_.erase(it->second.second);
                data_.erase(it);
            }
            hashes_.erase(key);
            lists_.erase(key);
            sets_.erase(key);
        } else if (cmd == "HSET") {
            std::string key, field, value;
            ss >> key >> field >> value;
            hashes_[key][field] = value;
            loaded++;
        } else if (cmd == "HDEL") {
            std::string key, field;
            ss >> key >> field;
            auto it = hashes_.find(key);
            if (it != hashes_.end()) {
                it->second.erase(field);
                if (it->second.empty()) hashes_.erase(it);
            }
        } else if (cmd == "LPUSH") {
            std::string key;
            ss >> key;
            std::string val;
            while (ss >> val) lists_[key].push_front(val);
            loaded++;
        } else if (cmd == "RPUSH") {
            std::string key;
            ss >> key;
            std::string val;
            while (ss >> val) lists_[key].push_back(val);
            loaded++;
        } else if (cmd == "LPOP") {
            std::string key;
            ss >> key;
            auto it = lists_.find(key);
            if (it != lists_.end() && !it->second.empty()) {
                it->second.pop_front();
                if (it->second.empty()) lists_.erase(it);
            }
        } else if (cmd == "RPOP") {
            std::string key;
            ss >> key;
            auto it = lists_.find(key);
            if (it != lists_.end() && !it->second.empty()) {
                it->second.pop_back();
                if (it->second.empty()) lists_.erase(it);
            }
        } else if (cmd == "SADD") {
            std::string key;
            ss >> key;
            std::string member;
            while (ss >> member) sets_[key].insert(member);
            loaded++;
        } else if (cmd == "SREM") {
            std::string key;
            ss >> key;
            std::string member;
            auto it = sets_.find(key);
            while (ss >> member) {
                if (it != sets_.end()) it->second.erase(member);
            }
            if (it != sets_.end() && it->second.empty()) sets_.erase(it);
        } else if (cmd == "ZADD") {
            std::string key;
            ss >> key;
            std::string score_str, member;
            while (ss >> score_str >> member) {
                zsets_[key][member] = std::stod(score_str);
            }
            loaded++;
        } else if (cmd == "FLUSHALL") {
            data_.clear();
            lru_list_.clear();
            hashes_.clear();
            lists_.clear();
            sets_.clear();
            zsets_.clear();
        }
        } catch (const std::exception&) {
            std::cerr << "AOF: skipping malformed/truncated entry" << std::endl;
            truncated++;
        }
    }
    std::cout << "AOF: loaded " << loaded << " entries";
    if (corrupted > 0) std::cout << " (" << corrupted << " corrupted entries skipped)";
    if (truncated > 0) std::cout << " (" << truncated << " malformed/truncated entries skipped)";
    std::cout << std::endl;
}

} // namespace litekv