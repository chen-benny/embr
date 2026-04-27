//
// Created by benny on 4/20/26.
//

#include "pull_cli.hpp"
#include "core/pull.hpp"
#include "tracker/tracker_client.hpp"
#include "transport/tcp_client.hpp"
#include "util/constants.hpp"
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_pull_usage() {
    std::cerr << "Usage: embr pull <token-or-ip> [--port PORT] [--tracker URL] [--out PATH]\n"
              << "\n"
              << "  <token-or-ip>   16 hex char token or IPv4 address\n"
              << "  --port PORT     sender port for direct IP mode (default 10007)\n"
              << "  --tracker URL   tracker URL; overrides EMBR_TRACKER env var\n"
              << "  --out PATH      output file path (default filename from sender)\n";
}

std::string resolve_tracker_url(const std::string& flag_value) {
    if (!flag_value.empty()) { return flag_value; }
    const char* env = ::getenv("EMBR_TRACKER");
    if (env && env[0] != '\0') { return std::string(env); }
    return {};
}

bool is_ipv4(const std::string& arg) {
    struct in_addr addr{};
    return ::inet_pton(AF_INET, arg.c_str(), &addr) == 1;
}

bool is_token(const std::string& arg) {
    if (arg.size() != TOKEN_SIZE) { return false; }
    return std::all_of(arg.begin(), arg.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); // hex chars
    });
}

}

int run_pull_cli(int argc, char* argv[]) {
    if (argc < 2) {
        print_pull_usage();
        return 1;
    }

    const std::string arg = argv[1];
    uint16_t port = EMBR_PORT;
    std::string tracker_url;
    std::string out_path;

    for (int i = 2; i < argc; i++) {
        const std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--tracker" && i + 1 < argc) {
            tracker_url = argv[++i];
        } else if (flag == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (flag == "--help") {
            print_pull_usage();
            return 0;
        } else {
            std::cerr << "embr pull: unknown flag: " << flag << "\n";
            print_pull_usage();
            return 1;
        }
    }

    tracker_url = resolve_tracker_url(tracker_url);

    try {
        std::string sender_ip;
        uint16_t sender_port;

        if (is_ipv4(arg)) { // direct mode
            sender_ip = arg;
            sender_port = port;
            std::cout << "[pull] direct mode: " << sender_ip
                      << ":" << sender_port << "\n";

        } else if (is_token(arg)) { // tracker mode
            if (tracker_url.empty()) {
                std::cerr << "[error] token provided but no tracker configured\n"
                          << "        set EMBR_TRACKER or pass --tracker URL\n";
                return 1;
            }
            std::cout << "[pull] resolving token: " << arg
                      << " tracker: " << tracker_url << "\n";
            auto [resolved_ip, resolved_port] = tracker_resolve(tracker_url, arg);
            sender_ip = resolved_ip;
            sender_port = resolved_port;
            std::cout << "[pull] resolved to " << sender_ip
                      << ":" << sender_port << "\n";

        } else {
            std::cerr << "[error] '" << arg
                      << "' is neither a valid IPv4 address nor a valid token\n";
            return 1;
        }

        auto conn = tcp_connect(sender_ip, sender_port);
        std::cout << "[pull] connect to " << sender_ip
                  << ":" << sender_port << "\n";
        run_pull(*conn, out_path);

    } catch (const std::exception& ex) {
        std::cerr << "[error] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
