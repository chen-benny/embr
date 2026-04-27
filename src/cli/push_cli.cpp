//
// Created by benny on 4/20/26.
//

#include "push_cli.hpp"

#include "core/hash.hpp"
#include "core/push.hpp"
#include "tracker/tracker_client.hpp"
#include "transport/tcp_server.hpp"
#include "util/socket_fd.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void print_push_usage() {
    std::cerr << "Usage: embr push <file> [--port PORT] [--tracker URL] [--ip IP]\n"
              << "\n"
              << "  --port PORT     listen port (default: 10007)\n"
              << "  --tracker URL   tracker URL; overrides EMBR_TRACKER env var\n"
              << "  --ip IP         override sender IP under NAT\n";
}

std::string resolve_tracker_url(const std::string& flag_values) {
    if (!flag_values.empty()) { return flag_values; }
    const char* env = ::getenv("EMBR_TRACKER");
    if (env && env[0] != '\0') { return std::string(env); }
    return {};
}

// Derive 16 hex char token from file (deterministic), first 8 bytes from SHA256 hashes
std::string derive_token(const std::vector<std::array<uint8_t, HASH_SIZE>>& chunk_hashes) {
    std::vector<uint8_t> concat;
    concat.reserve(chunk_hashes.size() * HASH_SIZE);
    for (const auto& hash : chunk_hashes) {
        concat.insert(concat.end(), hash.begin(), hash.end());
    }
    const auto digest = sha256_buf(concat.data(), concat.size());

    std::ostringstream oss;
    for (size_t i = 0; i < 8; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

}

int run_push_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_push_usage();
        return 1;
    }

    const std::string file_path = argv[1];
    uint16_t port = 10007;
    std::string tracker_url;
    std::string sender_ip;

    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--tracker" && i + 1 < argc) {
            tracker_url = argv[++i];
        } else if (flag == "--ip" && i + 1 < argc) {
            sender_ip = argv[++i];
        } else if (flag == "--help") {
            print_push_usage();
            return 0;
        } else {
            std::cerr << "embr push: unknown flag: " << flag << "\n";
            print_push_usage();
            return 1;
        }
    }

    tracker_url = resolve_tracker_url(tracker_url);
    if (!tracker_url.empty()) {
        std::cout << "[push] tracker: " << tracker_url << "\n";
    }

    try {
        FileMeta file_meta = precompute_meta(file_path);
        SocketFd file_fd{::open(file_path.c_str(), O_RDONLY)};
        if (file_fd.get() < 0) {
            throw std::runtime_error("embr push: failed to open file: " + file_path +
                                     " - " + std::strerror(errno));
        }

        const std::string token = derive_token(file_meta.chunk_hashes);

        SocketFd listen_fd = tcp_listen(port);
        if (listen_fd.get() < 0) {
            throw std::runtime_error("embr push: failed to listen on port " +
                                      std::to_string(port) + " - " + std::strerror(errno));
        }
        std::cout << "[push] listening on port " << port << "\n";

        // register with tracker after listen_fd is bound: port open before pull connection
        if (!tracker_url.empty()) {
            tracker_register(tracker_url, token, port); // v0.6: sender_ip is shadow, only for NAT
            std::cout << "[push] token: " << token << "\n";
            if (!sender_ip.empty()) {
                std::cout << "[push] sender ip: " << sender_ip << "\n";
            }
        }

        auto conn = tcp_accept(listen_fd.get());
        std::cout << "[push] connection established\n";

        run_push(*conn, std::move(file_fd), std::move(file_meta));

        if (!tracker_url.empty()) {
            tracker_unregister(tracker_url, token);
        }

    } catch (const std::exception& ex) {
        std::cerr << "[error] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
