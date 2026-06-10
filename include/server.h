#pragma once

#include "storage.h"
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
namespace litekv {

enum class Role { MASTER, REPLICA };

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

    void accept_loop();
    void handle_client(int client_fd);
    std::string process_command(std::string_view raw, bool from_master = false);
    void propagate_to_replicas(const std::string& cmd);
    void connect_to_master();
    void replica_loop(int master_fd);

#ifdef __linux__
    void epoll_loop();
    void handle_event(int fd, bool& is_new_conn);
#endif
};

} // namespace litekv