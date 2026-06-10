#include "server.h"
#include <iostream>
#include <csignal>
#include <string>

litekv::Server* g_server = nullptr;

void signal_handler(int) {
    std::cout << "\nShutting down LiteKV..." << std::endl;
    if (g_server) g_server->stop();
    exit(0);
}

int main(int argc, char* argv[]) {
    int port = 6380;
    std::string aof_path = "litekv.aof";
    litekv::Role role = litekv::Role::MASTER;
    std::string master_host = "";
    int master_port = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--aof" && i + 1 < argc) {
            aof_path = argv[++i];
        } else if (arg == "--replicaof" && i + 2 < argc) {
            role = litekv::Role::REPLICA;
            master_host = argv[++i];
            master_port = std::stoi(argv[++i]);
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        litekv::Server server(port, aof_path, role, master_host, master_port);
        g_server = &server;
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}