#include "storage.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace litekv {

Storage::Storage(const std::string& aof_path, size_t max_keys)
    : aof_path_(aof_path), max_keys_(max_keys) {
    aof_file_.open(aof_path_, std::ios::app);
    if (!aof_file_.is_open()) {
        std::cerr << "Warning: Could not open AOF file: " << aof_path_ << std::endl;
    }
}

bool Storage::is_expired(const Entry& entry) const {
    if (!entry.expires_at.has_value()) return false;
    return std::chrono::steady_clock::now() > entry.expires_at.value();
}

void Storage::append_aof(const std::string& line) {
    if (aof_file_.is_open()) {
        aof_file_ << line << "\n";
        aof_file_.flush();
    }
}

void Storage::touch(const std::string& key) {
    auto it = data_.find(key);
    if (it != data_.end()) {
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
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

void Storage::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove existing entry
    auto it = data_.find(key);
    if (it != data_.end()) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
    }

    evict_lru();

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

bool Storage::setnx(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = data_.find(key);
    if (it != data_.end() && is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        it = data_.end();
    }
    if (it != data_.end()) return false;

    evict_lru();
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
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    } else {
        evict_lru();
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
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
    } else {
        new_value = value;
        evict_lru();
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
    return hashes_.find(key) != hashes_.end();
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

void Storage::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    lru_list_.clear();
    hashes_.clear();
    append_aof("FLUSHALL");
}

size_t Storage::size() const {
    return data_.size();
}

std::vector<std::vector<std::string>> Storage::dump_commands() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    return commands;
}

void Storage::load_aof() {
    std::ifstream file(aof_path_);
    if (!file.is_open()) return;

    std::string line;
    int loaded = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

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
        } else if (cmd == "HSET") {
            std::string key, field, value;
            ss >> key >> field >> value;
            hashes_[key][field] = value;
            loaded++;
        } else if (cmd == "FLUSHALL") {
            data_.clear();
            lru_list_.clear();
            hashes_.clear();
        }
    }
    std::cout << "AOF: loaded " << loaded << " entries" << std::endl;
}

} // namespace litekv