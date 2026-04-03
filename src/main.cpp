#include <iostream>
#include <string>
#include <cstdint>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include "core/protocol.hpp"
#include "core/pull.hpp"
#include "core/push.hpp"
#include "transport/tcp_server.hpp"
#include "transport/tcp_client.hpp"
#include "util/socket_fd.hpp"

static constexpr uint16_t DEFAULT_PORT = 9000;

static void usage() {
    std::cerr << "Usage:\n"
              << " embr push <file> [--port PORT]\n"
              << " embr pull <ip> [--port PORT] [--out PATH]\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    std::string cmd = argv[1];
    std::string arg = argv[2];
    uint16_t port = DEFAULT_PORT;
    std::string out_path;

    // parse optional flags
    for (int i = 3; i < argc; i++) {
        std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::cerr << "\nUnknown flag: " << flag << "\n";
            usage();
            return 1;
        }
    }

    try {
        if (cmd == "push") {
            // gen filemeta immediately
            FileMeta file_meta = precompute_meta(arg);
            SocketFd file_fd{::open(arg.c_str(), O_RDONLY)};
            if (file_fd.get() < 0) {
                throw std::runtime_error("failed to open file: " +
                                         arg + " - " + std::strerror(errno));
            }
            SocketFd listen_fd = tcp_listen(port);
            std::cout << "[main] listening on port " << port << "\n";
            auto conn = tcp_accept(listen_fd.get());
            std::cout << "[main] connection established\n";
            run_push(*conn, std::move(file_fd), std::move(file_meta));
        } else if (cmd == "pull") {
            // transport set to TCP
            auto conn = tcp_connect(arg, port);
            std::cout << "[main] connected to " << arg << ":" << port << "\n";
            run_pull(*conn, out_path);
        } else {
            std::cerr << "\nUnknown command: " << cmd << "\n";
            usage();
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[error] " << ex.what() << "\n";
        return 1;
    }

    return 0;
}