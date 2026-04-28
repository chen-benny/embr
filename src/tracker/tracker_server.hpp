//
// Created by benny on 4/26/26.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct TrackerConfig {
    std::string bind_addr; // address to bind HTTP server
    uint16_t bind_port; // tracker server listening port
    std::chrono::minutes ttl; // token TTL before eviction
};

// Start the tracker HTTP server in foreground: block until SIGINT or SIGTERM
void run_tracker_server(const TrackerConfig& config);