#pragma once

#include "storage.h"
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
namespace litekv {

enum class Role { MASTER, REPLICA };

struct ClientTxState {
    bool in_multi = false;
    std::vector<std::string> queued;
    std::unordered_set<std::string> watched_keys;
    // Set to the storage version at the first WATCH call; if the version drifts
    // before EXEC, the whole transaction aborts (coarse-grained, not per-key).
    std::optional<uint64_t> watch_version;
    bool authenticated = false;
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
    static constexpr int kNumDbs = 16;

    int port_;
    int server_fd_;
    int master_fd_ = -1;
    std::atomic<bool> running_;
    std::vector<std::unique_ptr<Storage>> dbs_;
    std::atomic<Role> role_;
    std::string master_host_;
    int master_port_;

    std::unordered_map<int, int> client_db_;
    std::mutex client_db_mutex_;
    int repl_current_db_ = 0;
    int last_propagated_db_ = -1;
    std::atomic<bool> bgsave_in_progress_{false};
    std::atomic<bool> bgrewriteaof_in_progress_{false};

    std::vector<int> replicas_;
    std::mutex replicas_mutex_;
    std::atomic<long long> repl_offset_{0};
    std::unordered_map<int, long long> replica_acks_;

    static constexpr size_t kBacklogMaxBytes = 1 << 20;
    std::string backlog_buffer_;
    long long backlog_start_offset_ = 0;

    std::unordered_map<std::string, std::vector<int>> channel_subs_;
    std::vector<std::pair<std::string, int>> pattern_subs_;
    std::mutex pubsub_mutex_;

    std::unordered_map<std::string, std::string> config_;
    std::mutex config_mutex_;

    void accept_loop();
    void handle_client(int client_fd);
    void apply_idle_timeout(int client_fd);
    std::string process_command(std::string_view raw, bool from_master = false, int client_fd = -1);
    std::string dispatch_transactional(const std::string& msg, ClientTxState& tx, int client_fd);
    void propagate_to_replicas(const std::string& cmd, int db_idx);
    void send_full_resync(int fd);
    void append_to_backlog_locked(const std::string& data);
    void connect_to_master();
    int try_connect_to_master();
    void replica_loop(int master_fd);
    void replication_heartbeat_loop();
    long long subscriber_count_locked(int fd);
    void unsubscribe_all(int fd);
    int get_client_db(int client_fd);
    void set_client_db(int client_fd, int db_idx);
    void remove_client_db(int client_fd);

#ifdef __linux__
    void epoll_loop();
    void handle_event(int fd, bool& is_new_conn);
#endif
};

} // namespace litekv