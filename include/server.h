#pragma once

#include "storage.h"
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
namespace litekv {

enum class Role { MASTER, REPLICA };

struct ClientTxState {
    bool in_multi = false;
    std::vector<std::string> queued;
};

class Server {
public:
    Server(int port, const std::string& aof_path = "litekv.aof",
           Role role = Role::MASTER,
           const std::string& master_host = "",
           int master_port = 0);
    void start();
    void stop();

private:
    int port_;
    int server_fd_;
    int master_fd_ = -1;
    std::atomic<bool> running_;
    Storage storage_;
    Role role_;
    std::string master_host_;
    int master_port_;
    

    std::vector<int> replicas_;
    std::mutex replicas_mutex_;
    std::atomic<long long> repl_offset_{0};
    std::unordered_map<int, long long> replica_acks_;

    void accept_loop();
    void handle_client(int client_fd);
    std::string process_command(std::string_view raw, bool from_master = false);
    std::string dispatch_transactional(const std::string& msg, ClientTxState& tx);
    void propagate_to_replicas(const std::string& cmd);
    void send_full_resync(int fd);
    void connect_to_master();
    int try_connect_to_master();
    void replica_loop(int master_fd);

#ifdef __linux__
    void epoll_loop();
    void handle_event(int fd, bool& is_new_conn);
#endif
};

} // namespace litekv