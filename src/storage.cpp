#include "storage.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>

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

bool Storage::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    lru_list_.erase(it->second.second);
    data_.erase(it);
    append_aof("DEL " + key);
    return true;
}

bool Storage::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return false;
    if (is_expired(it->second.first)) {
        lru_list_.erase(it->second.second);
        data_.erase(it);
        return false;
    }
    return true;
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

void Storage::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    lru_list_.clear();
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
        } else if (cmd == "FLUSHALL") {
            data_.clear();
            lru_list_.clear();
        }
    }
    std::cout << "AOF: loaded " << loaded << " entries" << std::endl;
}

} // namespace litekv