#include "server.h"
#include "parser.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <algorithm>
#include <fcntl.h>
#include <cmath>
#include <cstdio>

#ifdef __linux__
#include <sys/epoll.h>
#endif

namespace litekv {

Server::Server(int port, const std::string& aof_path, Role role,
               const std::string& master_host, int master_port)
    : port_(port), server_fd_(-1), running_(false),
      storage_(aof_path), role_(role),
      master_host_(master_host), master_port_(master_port) {
    if (role_ == Role::MASTER) {
        storage_.load_aof();
    }
    config_ = {
        {"maxmemory", "0"},
        {"appendonly", "yes"},
        {"appendfsync", "everysec"},
        {"save", ""},
        {"timeout", "0"},
        {"requirepass", ""},
    };
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static std::string format_score(double value) {
    if (std::isfinite(value) && value == static_cast<double>(static_cast<long long>(value)) &&
        std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", value);
    return std::string(buf);
}

void Server::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) throw std::runtime_error("Failed to create socket");

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("Failed to bind");

    if (listen(server_fd_, 128) < 0)
        throw std::runtime_error("Failed to listen");

    running_ = true;

    std::string role_str = (role_ == Role::MASTER) ? "MASTER" : "REPLICA";
    std::cout << "LiteKV [" << role_str << "] started on port " << port_ << std::endl;

    if (role_ == Role::REPLICA) {
        std::thread(&Server::connect_to_master, this).detach();
    }

#ifdef __linux__
    if (role_ == Role::REPLICA) {
        std::cout << "Using thread-per-connection (replica)" << std::endl;
        accept_loop();
    } else {
        std::cout << "Using epoll event loop (master)" << std::endl;
        epoll_loop();
    }
#else
    std::cout << "Using thread-per-connection" << std::endl;
    accept_loop();
#endif
}

void Server::stop() {
    running_ = false;
    close(server_fd_);
}

#ifdef __linux__
void Server::epoll_loop() {
    set_nonblocking(server_fd_);

    int epfd = epoll_create1(0);
    if (epfd < 0) throw std::runtime_error("epoll_create1 failed");

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_, &ev);

    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    std::unordered_map<int, std::string> client_buffers;
    std::unordered_map<int, ClientTxState> client_tx;

    while (running_) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                sockaddr_in client_addr{};
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &len);
                if (client_fd < 0) continue;

                set_nonblocking(client_fd);
                epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
                client_buffers[client_fd] = "";
                client_tx[client_fd] = ClientTxState{};
            } else {
                char buf[4096];
                ssize_t bytes = recv(fd, buf, sizeof(buf) - 1, 0);
                if (bytes <= 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    client_buffers.erase(fd);
                    client_tx.erase(fd);
                    unsubscribe_all(fd);
                    std::lock_guard<std::mutex> lock(replicas_mutex_);
                    auto it = std::find(replicas_.begin(), replicas_.end(), fd);
                    if (it != replicas_.end()) replicas_.erase(it);
                    replica_acks_.erase(fd);
                    continue;
                }

                buf[bytes] = '\0';
                client_buffers[fd].append(buf, bytes);
                std::string& cbuf = client_buffers[fd];

                while (!cbuf.empty()) {
                    if (cbuf.find("REPLCONF") != std::string::npos) {
                        Command replconf = Parser::parse(cbuf);
                        cbuf.clear();
                        if (!replconf.args.empty() && replconf.args[0] == "ACK") {
                            if (replconf.args.size() >= 2) {
                                try {
                                    long long off = std::stoll(replconf.args[1]);
                                    std::lock_guard<std::mutex> lock(replicas_mutex_);
                                    replica_acks_[fd] = off;
                                } catch (...) {}
                            }
                            break;
                        }
                        std::lock_guard<std::mutex> lock(replicas_mutex_);
                        if (std::find(replicas_.begin(), replicas_.end(), fd) == replicas_.end()) {
                            replicas_.push_back(fd);
                        }
                        std::string resp = "+OK\r\n";
                        send(fd, resp.c_str(), resp.size(), 0);
                        send_full_resync(fd);
                        break;
                    }

                    if (cbuf[0] == '*') {
                        size_t end = cbuf.find("\r\n");
                        if (end == std::string::npos) break;
                        int count = std::stoi(cbuf.substr(1, end - 1));
                        size_t pos = end + 2;
                        bool complete = true;
                        for (int j = 0; j < count; j++) {
                            size_t dollar = cbuf.find("$", pos);
                            if (dollar == std::string::npos) { complete = false; break; }
                            size_t len_end = cbuf.find("\r\n", dollar);
                            if (len_end == std::string::npos) { complete = false; break; }
                            int len = std::stoi(cbuf.substr(dollar + 1, len_end - dollar - 1));
                            pos = len_end + 2 + len + 2;
                            if (pos > cbuf.size()) { complete = false; break; }
                        }
                        if (!complete) break;

                        std::string msg = cbuf.substr(0, pos);
                        cbuf.erase(0, pos);
                        std::string response = dispatch_transactional(msg, client_tx[fd], fd);
                        send(fd, response.c_str(), response.size(), 0);
                    } else {
                        size_t end = cbuf.find("\r\n");
                        if (end == std::string::npos) break;
                        std::string msg = cbuf.substr(0, end + 2);
                        cbuf.erase(0, end + 2);
                        std::string response = dispatch_transactional(msg, client_tx[fd], fd);
                        send(fd, response.c_str(), response.size(), 0);
                    }
                }
            }
        }
    }
    close(epfd);
}
#endif

void Server::accept_loop() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running_) std::cerr << "Accept error" << std::endl;
            continue;
        }
        std::thread(&Server::handle_client, this, client_fd).detach();
    }
}

std::string Server::dispatch_transactional(const std::string& msg, ClientTxState& tx, int client_fd) {
    Command cmd = Parser::parse(msg);

    if (cmd.name == "AUTH") {
        std::string requirepass;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            requirepass = config_["requirepass"];
        }
        if (requirepass.empty()) {
            return Parser::error_response("Client sent AUTH, but no password is set. Did you mean AUTH <username> <password>?");
        }
        if (cmd.args.size() != 1) return Parser::error_response("wrong number of arguments for AUTH");
        if (cmd.args[0] != requirepass) {
            tx.authenticated = false;
            return Parser::error_response("WRONGPASS invalid username-password pair or user is disabled.");
        }
        tx.authenticated = true;
        return Parser::ok_response();
    }
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (!config_["requirepass"].empty() && !tx.authenticated) {
            return Parser::error_response("NOAUTH Authentication required.");
        }
    }

    if (cmd.name == "MULTI") {
        if (tx.in_multi) return Parser::error_response("MULTI calls can not be nested");
        tx.in_multi = true;
        tx.queued.clear();
        return Parser::ok_response();
    }
    if (cmd.name == "DISCARD") {
        if (!tx.in_multi) return Parser::error_response("DISCARD without MULTI");
        tx.in_multi = false;
        tx.queued.clear();
        tx.watched_keys.clear();
        tx.watch_version.reset();
        return Parser::ok_response();
    }
    if (cmd.name == "WATCH") {
        if (tx.in_multi) return Parser::error_response("WATCH inside MULTI is not allowed");
        if (cmd.args.empty()) return Parser::error_response("wrong number of arguments for WATCH");
        if (!tx.watch_version.has_value()) tx.watch_version = storage_.version();
        for (const auto& key : cmd.args) tx.watched_keys.insert(key);
        return Parser::ok_response();
    }
    if (cmd.name == "UNWATCH") {
        tx.watched_keys.clear();
        tx.watch_version.reset();
        return Parser::ok_response();
    }
    if (cmd.name == "EXEC") {
        if (!tx.in_multi) return Parser::error_response("EXEC without MULTI");
        tx.in_multi = false;
        bool aborted = tx.watch_version.has_value() && storage_.version() != tx.watch_version.value();
        tx.watched_keys.clear();
        tx.watch_version.reset();
        if (aborted) {
            tx.queued.clear();
            return "*-1\r\n";
        }
        std::string resp = "*" + std::to_string(tx.queued.size()) + "\r\n";
        for (const auto& q : tx.queued) resp += process_command(q, false, client_fd);
        tx.queued.clear();
        return resp;
    }
    if (tx.in_multi) {
        if (cmd.name.empty()) return Parser::error_response("empty command");
        tx.queued.push_back(msg);
        return "+QUEUED\r\n";
    }
    return process_command(msg, false, client_fd);
}

void Server::handle_client(int client_fd) {
    char buffer[4096];
    ClientTxState tx;
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            unsubscribe_all(client_fd);
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            auto it = std::find(replicas_.begin(), replicas_.end(), client_fd);
            if (it != replicas_.end()) replicas_.erase(it);
            replica_acks_.erase(client_fd);
            break;
        }

        std::string_view raw(buffer, bytes);

        if (raw.find("REPLCONF") != std::string_view::npos) {
            Command replconf = Parser::parse(raw);
            if (!replconf.args.empty() && replconf.args[0] == "ACK") {
                if (replconf.args.size() >= 2) {
                    try {
                        long long off = std::stoll(replconf.args[1]);
                        std::lock_guard<std::mutex> lock(replicas_mutex_);
                        replica_acks_[client_fd] = off;
                    } catch (...) {}
                }
                continue;
            }
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            if (std::find(replicas_.begin(), replicas_.end(), client_fd) == replicas_.end()) {
                replicas_.push_back(client_fd);
            }
            std::string resp = "+OK\r\n";
            send(client_fd, resp.c_str(), resp.size(), 0);
            send_full_resync(client_fd);
            continue;
        }

        std::string response = dispatch_transactional(std::string(raw), tx, client_fd);
        send(client_fd, response.c_str(), response.size(), 0);
    }
    close(client_fd);
}

void Server::propagate_to_replicas(const std::string& cmd) {
    repl_offset_ += static_cast<long long>(cmd.size());
    std::lock_guard<std::mutex> lock(replicas_mutex_);
    for (int fd : replicas_) {
        send(fd, cmd.c_str(), cmd.size(), 0);
    }
}

void Server::send_full_resync(int fd) {
    for (const auto& cmd : storage_.dump_commands()) {
        std::string encoded = Parser::encode_command(cmd);
        send(fd, encoded.c_str(), encoded.size(), 0);
    }
}

// Caller must hold pubsub_mutex_.
long long Server::subscriber_count_locked(int fd) {
    long long count = 0;
    for (const auto& [channel, subs] : channel_subs_) {
        if (std::find(subs.begin(), subs.end(), fd) != subs.end()) count++;
    }
    for (const auto& [pattern, sub_fd] : pattern_subs_) {
        if (sub_fd == fd) count++;
    }
    return count;
}

void Server::unsubscribe_all(int fd) {
    std::lock_guard<std::mutex> lock(pubsub_mutex_);
    for (auto it = channel_subs_.begin(); it != channel_subs_.end(); ) {
        auto& subs = it->second;
        subs.erase(std::remove(subs.begin(), subs.end(), fd), subs.end());
        if (subs.empty()) it = channel_subs_.erase(it);
        else ++it;
    }
    pattern_subs_.erase(
        std::remove_if(pattern_subs_.begin(), pattern_subs_.end(),
                        [fd](const auto& p) { return p.second == fd; }),
        pattern_subs_.end());
}

std::string Server::process_command(std::string_view raw, bool from_master, int client_fd) {
    Command cmd = Parser::parse(raw);

    if (cmd.name.empty()) return Parser::error_response("empty command");

    if (role_ == Role::REPLICA && !from_master) {
        if (cmd.name == "SET" || cmd.name == "DEL" || cmd.name == "FLUSHALL" ||
            cmd.name == "INCR" || cmd.name == "INCRBY" || cmd.name == "DECRBY" ||
            cmd.name == "APPEND" || cmd.name == "MSET" || cmd.name == "GETSET" ||
            cmd.name == "SETNX" || cmd.name == "RENAME" || cmd.name == "COPY" ||
            cmd.name == "EXPIRE" || cmd.name == "PERSIST" || cmd.name == "PEXPIRE" ||
            cmd.name == "HSET" || cmd.name == "HDEL" ||
            cmd.name == "LPUSH" || cmd.name == "RPUSH" ||
            cmd.name == "LPOP" || cmd.name == "RPOP" ||
            cmd.name == "SADD" || cmd.name == "SREM" || cmd.name == "ZADD") {
            return Parser::error_response("READONLY You can't write against a read only replica");
        }
    }

    if (cmd.name == "PING") return "+PONG\r\n";

    else if (cmd.name == "SET") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for SET");
        int ttl = -1;
        if (cmd.args.size() == 4 &&
            (cmd.args[2] == "EX" || cmd.args[2] == "ex")) {
            ttl = std::stoi(cmd.args[3]);
        }
        storage_.set(cmd.args[0], cmd.args[1], ttl);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::ok_response();
    }
    else if (cmd.name == "GET") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for GET");
        auto val = storage_.get(cmd.args[0]);
        if (!val.has_value()) return Parser::null_response();
        return Parser::bulk_response(val.value());
    }
    else if (cmd.name == "DEL") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for DEL");
        bool deleted = storage_.del(cmd.args[0]);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(deleted ? 1 : 0);
    }
    else if (cmd.name == "INCR") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for INCR");
        auto result = storage_.incrby(cmd.args[0], 1);
        if (!result.has_value()) return Parser::error_response("value is not an integer or out of range");
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(result.value());
    }
    else if (cmd.name == "INCRBY" || cmd.name == "DECRBY") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for " + cmd.name);
        long long delta;
        try {
            size_t pos;
            delta = std::stoll(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }
        if (cmd.name == "DECRBY") delta = -delta;
        auto result = storage_.incrby(cmd.args[0], delta);
        if (!result.has_value()) return Parser::error_response("value is not an integer or out of range");
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(result.value());
    }
    else if (cmd.name == "APPEND") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for APPEND");
        long long new_len = storage_.append(cmd.args[0], cmd.args[1]);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(new_len);
    }
    else if (cmd.name == "MSET") {
        if (cmd.args.empty() || cmd.args.size() % 2 != 0)
            return Parser::error_response("wrong number of arguments for MSET");
        std::vector<std::pair<std::string, std::string>> pairs;
        for (size_t i = 0; i < cmd.args.size(); i += 2) {
            pairs.emplace_back(cmd.args[i], cmd.args[i + 1]);
        }
        storage_.mset(pairs);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::ok_response();
    }
    else if (cmd.name == "GETSET") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for GETSET");
        auto old_val = storage_.getset(cmd.args[0], cmd.args[1]);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        if (!old_val.has_value()) return Parser::null_response();
        return Parser::bulk_response(old_val.value());
    }
    else if (cmd.name == "SETNX") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for SETNX");
        bool set = storage_.setnx(cmd.args[0], cmd.args[1]);
        if (set && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(set ? 1 : 0);
    }
    else if (cmd.name == "RENAME") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for RENAME");
        bool renamed = storage_.rename(cmd.args[0], cmd.args[1]);
        if (!renamed) return Parser::error_response("no such key");
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::ok_response();
    }
    else if (cmd.name == "COPY") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for COPY");
        bool replace = false;
        for (size_t i = 2; i < cmd.args.size(); i++) {
            std::string opt = cmd.args[i];
            std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);
            if (opt == "REPLACE") replace = true;
        }
        bool copied = storage_.copy(cmd.args[0], cmd.args[1], replace);
        if (copied && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(copied ? 1 : 0);
    }
    else if (cmd.name == "TYPE") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for TYPE");
        if (storage_.hlen(cmd.args[0]) > 0) return std::string("+hash\r\n");
        if (storage_.llen(cmd.args[0]) > 0) return std::string("+list\r\n");
        if (!storage_.smembers(cmd.args[0]).empty()) return std::string("+set\r\n");
        return storage_.exists(cmd.args[0]) ? std::string("+string\r\n") : std::string("+none\r\n");
    }
    else if (cmd.name == "MGET") {
        if (cmd.args.empty()) return Parser::error_response("wrong number of arguments for MGET");
        return Parser::array_response(storage_.mget(cmd.args));
    }
    else if (cmd.name == "STRLEN") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for STRLEN");
        return Parser::integer_response(storage_.strlen(cmd.args[0]));
    }
    else if (cmd.name == "HSET") {
        if (cmd.args.size() < 3 || (cmd.args.size() - 1) % 2 != 0)
            return Parser::error_response("wrong number of arguments for HSET");
        long long added = 0;
        for (size_t i = 1; i + 1 < cmd.args.size(); i += 2) {
            if (storage_.hset(cmd.args[0], cmd.args[i], cmd.args[i + 1])) added++;
        }
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(added);
    }
    else if (cmd.name == "HGET") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for HGET");
        auto val = storage_.hget(cmd.args[0], cmd.args[1]);
        if (!val.has_value()) return Parser::null_response();
        return Parser::bulk_response(val.value());
    }
    else if (cmd.name == "HDEL") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for HDEL");
        std::vector<std::string> fields(cmd.args.begin() + 1, cmd.args.end());
        long long removed = storage_.hdel(cmd.args[0], fields);
        if (removed > 0 && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(removed);
    }
    else if (cmd.name == "HGETALL") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for HGETALL");
        auto pairs = storage_.hgetall(cmd.args[0]);
        std::vector<std::optional<std::string>> flat;
        flat.reserve(pairs.size() * 2);
        for (const auto& [field, value] : pairs) {
            flat.push_back(field);
            flat.push_back(value);
        }
        return Parser::array_response(flat);
    }
    else if (cmd.name == "HEXISTS") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for HEXISTS");
        return Parser::integer_response(storage_.hexists(cmd.args[0], cmd.args[1]) ? 1 : 0);
    }
    else if (cmd.name == "HLEN") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for HLEN");
        return Parser::integer_response(storage_.hlen(cmd.args[0]));
    }
    else if (cmd.name == "HKEYS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for HKEYS");
        auto fields = storage_.hkeys(cmd.args[0]);
        std::vector<std::optional<std::string>> values(fields.begin(), fields.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "HVALS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for HVALS");
        auto values_vec = storage_.hvals(cmd.args[0]);
        std::vector<std::optional<std::string>> values(values_vec.begin(), values_vec.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "LPUSH" || cmd.name == "RPUSH") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for " + cmd.name);
        std::vector<std::string> values(cmd.args.begin() + 1, cmd.args.end());
        long long len = (cmd.name == "LPUSH")
            ? storage_.lpush(cmd.args[0], values)
            : storage_.rpush(cmd.args[0], values);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(len);
    }
    else if (cmd.name == "LPOP" || cmd.name == "RPOP") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for " + cmd.name);
        auto val = (cmd.name == "LPOP") ? storage_.lpop(cmd.args[0]) : storage_.rpop(cmd.args[0]);
        if (val.has_value() && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        if (!val.has_value()) return Parser::null_response();
        return Parser::bulk_response(val.value());
    }
    else if (cmd.name == "LRANGE") {
        if (cmd.args.size() < 3) return Parser::error_response("wrong number of arguments for LRANGE");
        long long start, stop;
        try {
            size_t pos;
            start = std::stoll(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
            stop = std::stoll(cmd.args[2], &pos);
            if (pos != cmd.args[2].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }
        auto items = storage_.lrange(cmd.args[0], start, stop);
        std::vector<std::optional<std::string>> values(items.begin(), items.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "LLEN") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for LLEN");
        return Parser::integer_response(storage_.llen(cmd.args[0]));
    }
    else if (cmd.name == "SADD") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for SADD");
        std::vector<std::string> members(cmd.args.begin() + 1, cmd.args.end());
        long long added = storage_.sadd(cmd.args[0], members);
        if (added > 0 && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(added);
    }
    else if (cmd.name == "SREM") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for SREM");
        std::vector<std::string> members(cmd.args.begin() + 1, cmd.args.end());
        long long removed = storage_.srem(cmd.args[0], members);
        if (removed > 0 && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(removed);
    }
    else if (cmd.name == "SMEMBERS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for SMEMBERS");
        auto members = storage_.smembers(cmd.args[0]);
        std::vector<std::optional<std::string>> values(members.begin(), members.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "SISMEMBER") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for SISMEMBER");
        return Parser::integer_response(storage_.sismember(cmd.args[0], cmd.args[1]) ? 1 : 0);
    }
    else if (cmd.name == "SCARD") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for SCARD");
        return Parser::integer_response(storage_.scard(cmd.args[0]));
    }
    else if (cmd.name == "ZADD") {
        if (cmd.args.size() < 3 || (cmd.args.size() - 1) % 2 != 0) {
            return Parser::error_response("wrong number of arguments for ZADD");
        }
        std::vector<std::pair<double, std::string>> members;
        for (size_t i = 1; i + 1 < cmd.args.size(); i += 2) {
            double score;
            try {
                size_t pos;
                score = std::stod(cmd.args[i], &pos);
                if (pos != cmd.args[i].size()) return Parser::error_response("value is not a valid float");
            } catch (...) {
                return Parser::error_response("value is not a valid float");
            }
            members.emplace_back(score, cmd.args[i + 1]);
        }
        long long added = storage_.zadd(cmd.args[0], members);
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(added);
    }
    else if (cmd.name == "ZSCORE") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for ZSCORE");
        auto score = storage_.zscore(cmd.args[0], cmd.args[1]);
        if (!score.has_value()) return Parser::null_response();
        return Parser::bulk_response(format_score(score.value()));
    }
    else if (cmd.name == "ZRANGE") {
        if (cmd.args.size() < 3) return Parser::error_response("wrong number of arguments for ZRANGE");
        long long start, stop;
        try {
            size_t pos;
            start = std::stoll(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
            stop = std::stoll(cmd.args[2], &pos);
            if (pos != cmd.args[2].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }
        auto items = storage_.zrange(cmd.args[0], start, stop);
        std::vector<std::optional<std::string>> values(items.begin(), items.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "EXISTS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for EXISTS");
        return Parser::integer_response(storage_.exists(cmd.args[0]) ? 1 : 0);
    }
    else if (cmd.name == "TTL") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for TTL");
        return Parser::integer_response(storage_.ttl(cmd.args[0]));
    }
    else if (cmd.name == "EXPIRE") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for EXPIRE");
        int seconds;
        try {
            size_t pos;
            seconds = std::stoi(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }
        bool ok = storage_.expire(cmd.args[0], seconds);
        if (ok && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(ok ? 1 : 0);
    }
    else if (cmd.name == "PERSIST") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for PERSIST");
        bool ok = storage_.persist(cmd.args[0]);
        if (ok && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(ok ? 1 : 0);
    }
    else if (cmd.name == "PEXPIRE") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for PEXPIRE");
        long long millis;
        try {
            size_t pos;
            millis = std::stoll(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }
        bool ok = storage_.pexpire(cmd.args[0], millis);
        if (ok && role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(ok ? 1 : 0);
    }
    else if (cmd.name == "PTTL") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for PTTL");
        return Parser::integer_response(storage_.pttl(cmd.args[0]));
    }
    else if (cmd.name == "KEYS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for KEYS");
        auto matched = storage_.keys(cmd.args[0]);
        std::vector<std::optional<std::string>> values(matched.begin(), matched.end());
        return Parser::array_response(values);
    }
    else if (cmd.name == "RANDOMKEY") {
        auto key = storage_.randomkey();
        if (!key.has_value()) return Parser::null_response();
        return Parser::bulk_response(key.value());
    }
    else if (cmd.name == "SCAN") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for SCAN");
        size_t cursor;
        try {
            size_t pos;
            cursor = std::stoull(cmd.args[0], &pos);
            if (pos != cmd.args[0].size()) return Parser::error_response("invalid cursor");
        } catch (...) {
            return Parser::error_response("invalid cursor");
        }
        std::string pattern = "*";
        size_t count = 10;
        for (size_t i = 1; i + 1 < cmd.args.size(); i += 2) {
            std::string opt = cmd.args[i];
            std::transform(opt.begin(), opt.end(), opt.begin(), ::toupper);
            if (opt == "MATCH") {
                pattern = cmd.args[i + 1];
            } else if (opt == "COUNT") {
                try {
                    count = std::stoull(cmd.args[i + 1]);
                } catch (...) {
                    return Parser::error_response("value is not an integer or out of range");
                }
            }
        }
        auto [next_cursor, matched] = storage_.scan(cursor, pattern, count);
        std::string out = "*2\r\n";
        out += Parser::bulk_response(std::to_string(next_cursor));
        std::vector<std::optional<std::string>> values(matched.begin(), matched.end());
        out += Parser::array_response(values);
        return out;
    }
    else if (cmd.name == "FLUSHALL") {
        storage_.flush();
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::ok_response();
    }
    else if (cmd.name == "INFO") {
        std::string role_str = (role_ == Role::MASTER) ? "master" : "slave";
        std::string offset_field = (role_ == Role::MASTER) ? "master_repl_offset" : "slave_repl_offset";
        return Parser::bulk_response("role:" + role_str + "\r\nkeys:" +
                                     std::to_string(storage_.size()) + "\r\n" +
                                     offset_field + ":" + std::to_string(repl_offset_.load()));
    }
    else if (cmd.name == "DBSIZE") {
        return Parser::integer_response(static_cast<int>(storage_.size()));
    }
    else if (cmd.name == "WAIT") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for WAIT");
        int num_replicas;
        long long timeout_ms;
        try {
            size_t pos;
            num_replicas = std::stoi(cmd.args[0], &pos);
            if (pos != cmd.args[0].size()) return Parser::error_response("value is not an integer or out of range");
            timeout_ms = std::stoll(cmd.args[1], &pos);
            if (pos != cmd.args[1].size()) return Parser::error_response("value is not an integer or out of range");
        } catch (...) {
            return Parser::error_response("value is not an integer or out of range");
        }

        long long target_offset = repl_offset_.load();
        auto count_acked = [this, target_offset]() {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            int n = 0;
            for (const auto& [fd, offset] : replica_acks_) {
                if (offset >= target_offset) n++;
            }
            return n;
        };

        auto start = std::chrono::steady_clock::now();
        int acked = count_acked();
        while (acked < num_replicas) {
            if (timeout_ms > 0 &&
                std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeout_ms)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            acked = count_acked();
        }
        return Parser::integer_response(acked);
    }
    else if (cmd.name == "SUBSCRIBE") {
        if (cmd.args.empty() || client_fd < 0) return Parser::error_response("wrong number of arguments for SUBSCRIBE");
        std::string resp;
        std::lock_guard<std::mutex> lock(pubsub_mutex_);
        for (const auto& channel : cmd.args) {
            auto& subs = channel_subs_[channel];
            if (std::find(subs.begin(), subs.end(), client_fd) == subs.end()) {
                subs.push_back(client_fd);
            }
            resp += "*3\r\n$9\r\nsubscribe\r\n" + Parser::bulk_response(channel) +
                    Parser::integer_response(subscriber_count_locked(client_fd));
        }
        return resp;
    }
    else if (cmd.name == "UNSUBSCRIBE") {
        if (client_fd < 0) return Parser::error_response("UNSUBSCRIBE not allowed in this context");
        std::lock_guard<std::mutex> lock(pubsub_mutex_);
        std::vector<std::string> channels = cmd.args;
        if (channels.empty()) {
            for (const auto& [channel, subs] : channel_subs_) {
                if (std::find(subs.begin(), subs.end(), client_fd) != subs.end()) channels.push_back(channel);
            }
        }
        if (channels.empty()) {
            return "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:" +
                   std::to_string(subscriber_count_locked(client_fd)) + "\r\n";
        }
        std::string resp;
        for (const auto& channel : channels) {
            auto it = channel_subs_.find(channel);
            if (it != channel_subs_.end()) {
                auto& subs = it->second;
                subs.erase(std::remove(subs.begin(), subs.end(), client_fd), subs.end());
                if (subs.empty()) channel_subs_.erase(it);
            }
            resp += "*3\r\n$11\r\nunsubscribe\r\n" + Parser::bulk_response(channel) +
                    Parser::integer_response(subscriber_count_locked(client_fd));
        }
        return resp;
    }
    else if (cmd.name == "PUBLISH") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for PUBLISH");
        const std::string& channel = cmd.args[0];
        const std::string& message = cmd.args[1];
        long long receivers = 0;
        std::string payload = "*3\r\n$7\r\nmessage\r\n" + Parser::bulk_response(channel) +
                               Parser::bulk_response(message);
        {
            std::lock_guard<std::mutex> lock(pubsub_mutex_);
            auto it = channel_subs_.find(channel);
            if (it != channel_subs_.end()) {
                for (int sub_fd : it->second) {
                    send(sub_fd, payload.c_str(), payload.size(), 0);
                    receivers++;
                }
            }
            for (const auto& [pattern, sub_fd] : pattern_subs_) {
                if (Storage::glob_match(pattern, channel)) {
                    std::string pmsg = "*4\r\n$8\r\npmessage\r\n" + Parser::bulk_response(pattern) +
                                        Parser::bulk_response(channel) + Parser::bulk_response(message);
                    send(sub_fd, pmsg.c_str(), pmsg.size(), 0);
                    receivers++;
                }
            }
        }
        if (role_ == Role::MASTER) {
            std::string raw_copy(raw);
            propagate_to_replicas(raw_copy);
        }
        return Parser::integer_response(receivers);
    }
    else if (cmd.name == "PSUBSCRIBE") {
        if (cmd.args.empty() || client_fd < 0) return Parser::error_response("wrong number of arguments for PSUBSCRIBE");
        std::string resp;
        std::lock_guard<std::mutex> lock(pubsub_mutex_);
        for (const auto& pattern : cmd.args) {
            bool already = false;
            for (const auto& [p, fd] : pattern_subs_) {
                if (p == pattern && fd == client_fd) { already = true; break; }
            }
            if (!already) pattern_subs_.emplace_back(pattern, client_fd);
            resp += "*3\r\n$10\r\npsubscribe\r\n" + Parser::bulk_response(pattern) +
                    Parser::integer_response(subscriber_count_locked(client_fd));
        }
        return resp;
    }
    else if (cmd.name == "PUNSUBSCRIBE") {
        if (client_fd < 0) return Parser::error_response("PUNSUBSCRIBE not allowed in this context");
        std::lock_guard<std::mutex> lock(pubsub_mutex_);
        std::vector<std::string> patterns = cmd.args;
        if (patterns.empty()) {
            for (const auto& [p, fd] : pattern_subs_) {
                if (fd == client_fd) patterns.push_back(p);
            }
        }
        if (patterns.empty()) {
            return "*3\r\n$12\r\npunsubscribe\r\n$-1\r\n:" +
                   std::to_string(subscriber_count_locked(client_fd)) + "\r\n";
        }
        std::string resp;
        for (const auto& pattern : patterns) {
            pattern_subs_.erase(
                std::remove_if(pattern_subs_.begin(), pattern_subs_.end(),
                                [&](const auto& entry) {
                                    return entry.first == pattern && entry.second == client_fd;
                                }),
                pattern_subs_.end());
            resp += "*3\r\n$12\r\npunsubscribe\r\n" + Parser::bulk_response(pattern) +
                    Parser::integer_response(subscriber_count_locked(client_fd));
        }
        return resp;
    }
    else if (cmd.name == "CONFIG") {
        if (cmd.args.size() < 2) return Parser::error_response("wrong number of arguments for CONFIG");
        std::string sub = cmd.args[0];
        std::transform(sub.begin(), sub.end(), sub.begin(), ::toupper);
        if (sub == "GET") {
            std::lock_guard<std::mutex> lock(config_mutex_);
            std::vector<std::optional<std::string>> flat;
            for (const auto& [key, value] : config_) {
                if (Storage::glob_match(cmd.args[1], key)) {
                    flat.push_back(key);
                    flat.push_back(value);
                }
            }
            return Parser::array_response(flat);
        } else if (sub == "SET") {
            if (cmd.args.size() < 3) return Parser::error_response("wrong number of arguments for CONFIG SET");
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_[cmd.args[1]] = cmd.args[2];
            return Parser::ok_response();
        }
        return Parser::error_response("unknown CONFIG subcommand '" + cmd.args[0] + "'");
    }

    return Parser::error_response("unknown command '" + cmd.name + "'");
}

int Server::try_connect_to_master() {
    int fd = -1;
    int retries = 10;

    while (retries-- > 0 && running_) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }

        struct addrinfo hints{}, *res;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(master_port_);
        int err = getaddrinfo(master_host_.c_str(), port_str.c_str(), &hints, &res);
        if (err != 0) {
            close(fd); fd = -1;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd;
        }

        freeaddrinfo(res);
        close(fd); fd = -1;
        std::cerr << "Replica: retrying connection to master..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    return -1;
}

void Server::connect_to_master() {
    const int max_backoff_seconds = 30;
    int backoff_seconds = 1;

    while (running_) {
        int fd = try_connect_to_master();

        if (fd < 0) {
            std::cerr << "Replica: failed to connect to master after retries, backing off "
                      << backoff_seconds << "s" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
            backoff_seconds = std::min(backoff_seconds * 2, max_backoff_seconds);
            continue;
        }

        backoff_seconds = 1;

        std::string replconf = "*1\r\n$8\r\nREPLCONF\r\n";
        send(fd, replconf.c_str(), replconf.size(), 0);

        char buf[64];
        recv(fd, buf, sizeof(buf), 0);

        master_fd_ = fd;
        std::cout << "Replica connected to master " << master_host_ << ":" << master_port_ << std::endl;

        replica_loop(fd);
        master_fd_ = -1;

        if (!running_) break;

        std::cerr << "Replica: lost connection to master, reconnecting in "
                  << backoff_seconds << "s" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
        backoff_seconds = std::min(backoff_seconds * 2, max_backoff_seconds);
    }
}

void Server::replica_loop(int master_fd) {
    char chunk[4096];
    std::string buffer;
    while (running_) {
        ssize_t bytes = recv(master_fd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            std::cerr << "Replica: lost connection to master" << std::endl;
            break;
        }
        buffer.append(chunk, bytes);

        while (!buffer.empty()) {
            if (buffer[0] == '*') {
                size_t end = buffer.find("\r\n");
                if (end == std::string::npos) break;
                int count = std::stoi(buffer.substr(1, end - 1));
                size_t pos = end + 2;
                bool complete = true;
                for (int j = 0; j < count; j++) {
                    size_t dollar = buffer.find('$', pos);
                    if (dollar == std::string::npos) { complete = false; break; }
                    size_t len_end = buffer.find("\r\n", dollar);
                    if (len_end == std::string::npos) { complete = false; break; }
                    int len = std::stoi(buffer.substr(dollar + 1, len_end - dollar - 1));
                    pos = len_end + 2 + len + 2;
                    if (pos > buffer.size()) { complete = false; break; }
                }
                if (!complete) break;

                std::string msg = buffer.substr(0, pos);
                buffer.erase(0, pos);
                process_command(msg, true);
                repl_offset_ += static_cast<long long>(msg.size());
            } else {
                size_t end = buffer.find("\r\n");
                if (end == std::string::npos) break;
                std::string msg = buffer.substr(0, end + 2);
                buffer.erase(0, end + 2);
                process_command(msg, true);
                repl_offset_ += static_cast<long long>(msg.size());
            }

            std::string ack = Parser::encode_command(
                {"REPLCONF", "ACK", std::to_string(repl_offset_.load())});
            send(master_fd, ack.c_str(), ack.size(), 0);
        }
    }
    close(master_fd);
}

} // namespace litekv