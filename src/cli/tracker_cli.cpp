//
// Created by benny on 4/20/26.
//

#include "tracker_cli.hpp"
#include "tracker/tracker_server.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

void print_tracker_usage() {
    std::cerr << "Usage: embr tracker [--bind ADDR] [--port PORT] [--ttl MINUTES]\n"
              << "\n"
              << "  --bind ADDR     bind address (default: 0.0.0.0)\n"
              << "  --port PORT     listening port (default: 10009)\n"
              << "  --ttl MINUTES   token TTL in minutes (default 10)\n";
}

}

int run_tracker_cli(int argc, char* argv[]) {
    tracker_config config{
        .bind_addr = "0.0.0.0",
        .bind_port = 10009,
        .ttl = std::chrono::minutes(10),
    };

    for (int i = 1; i < argc; i++) {
        const std::string flag = argv[i];
        if (flag == "--bind" && i + 1 < argc) {
            config.bind_addr = argv[++i];
        } else if (flag == "--port" && i + 1 < argc) {
            config.bind_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--ttl" && i + 1 < argc) {
            config.ttl = std::chrono::minutes(std::stoi(argv[++i]));
        } else if (flag == "--help") {
            print_tracker_usage();
            return 0;
        } else {
            std::cerr << "embr tracker: unknown flag: " << flag << "\n";
            print_tracker_usage();
            return 1;
        }
    }

    run_tracker_server(config);
    return 0;
}