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
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
            } else {
                char buf[4096];
                ssize_t bytes = recv(fd, buf, sizeof(buf) - 1, 0);
                if (bytes <= 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    client_buffers.erase(fd);
                    std::lock_guard<std::mutex> lock(replicas_mutex_);
                    auto it = std::find(replicas_.begin(), replicas_.end(), fd);
                    if (it != replicas_.end()) replicas_.erase(it);
                    continue;
                }

                buf[bytes] = '\0';
                client_buffers[fd].append(buf, bytes);
                std::string& cbuf = client_buffers[fd];

                while (!cbuf.empty()) {
                    if (cbuf.find("REPLCONF") != std::string::npos) {
                        std::lock_guard<std::mutex> lock(replicas_mutex_);
                        if (std::find(replicas_.begin(), replicas_.end(), fd) == replicas_.end()) {
                            replicas_.push_back(fd);
                        }
                        std::string resp = "+OK\r\n";
                        send(fd, resp.c_str(), resp.size(), 0);
                        send_full_resync(fd);
                        cbuf.clear();
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
                        std::string response = process_command(msg);
                        send(fd, response.c_str(), response.size(), 0);
                    } else {
                        size_t end = cbuf.find("\r\n");
                        if (end == std::string::npos) break;
                        std::string msg = cbuf.substr(0, end + 2);
                        cbuf.erase(0, end + 2);
                        std::string response = process_command(msg);
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

void Server::handle_client(int client_fd) {
    char buffer[4096];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            auto it = std::find(replicas_.begin(), replicas_.end(), client_fd);
            if (it != replicas_.end()) replicas_.erase(it);
            break;
        }

        std::string_view raw(buffer, bytes);

        if (raw.find("REPLCONF") != std::string_view::npos) {
            std::lock_guard<std::mutex> lock(replicas_mutex_);
            replicas_.push_back(client_fd);
            std::string resp = "+OK\r\n";
            send(client_fd, resp.c_str(), resp.size(), 0);
            send_full_resync(client_fd);
            continue;
        }

        std::string response = process_command(raw);
        send(client_fd, response.c_str(), response.size(), 0);
    }
    close(client_fd);
}

void Server::propagate_to_replicas(const std::string& cmd) {
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

std::string Server::process_command(std::string_view raw, bool from_master) {
    Command cmd = Parser::parse(raw);

    if (cmd.name.empty()) return Parser::error_response("empty command");

    if (role_ == Role::REPLICA && !from_master) {
        if (cmd.name == "SET" || cmd.name == "DEL" || cmd.name == "FLUSHALL" ||
            cmd.name == "INCR" || cmd.name == "INCRBY" || cmd.name == "DECRBY" ||
            cmd.name == "APPEND") {
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
    else if (cmd.name == "EXISTS") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for EXISTS");
        return Parser::integer_response(storage_.exists(cmd.args[0]) ? 1 : 0);
    }
    else if (cmd.name == "TTL") {
        if (cmd.args.size() < 1) return Parser::error_response("wrong number of arguments for TTL");
        return Parser::integer_response(storage_.ttl(cmd.args[0]));
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
        return Parser::bulk_response("role:" + role_str + "\r\nkeys:" +
                                     std::to_string(storage_.size()));
    }
    else if (cmd.name == "DBSIZE") {
        return Parser::integer_response(static_cast<int>(storage_.size()));
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
            } else {
                size_t end = buffer.find("\r\n");
                if (end == std::string::npos) break;
                std::string msg = buffer.substr(0, end + 2);
                buffer.erase(0, end + 2);
                process_command(msg, true);
            }
        }
    }
    close(master_fd);
}

} // namespace litekv