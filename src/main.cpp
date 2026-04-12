#include "core/protocol.hpp"
#include "core/pull.hpp"
#include "core/push.hpp"
#include "transport//udp_data_server.hpp"
#include "transport/tcp_client.hpp"
#include "transport/tcp_server.hpp"
#include "transport/udp_data_client.hpp"
#include "util/socket_fd.hpp"

#include <fcntl.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

static constexpr uint16_t DEFAULT_PORT        = 9000;
static constexpr uint16_t UDP_DATA_PORT_OFFSET = 1;

static void usage() {
    std::cerr << "Usage:\n"
              << "  embr push <file> [--port PORT] [--tcp]\n"
              << "  embr pull <ip>   [--port PORT] [--out PATH] [--tcp]\n"
              << "\n"
              << "  Default transport: TCP control (port) + UDP data (port+1)\n"
              << "  --tcp: use TCP fallback for both planes\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        usage();
        return 1;
    }

    std::string cmd      = argv[1];
    std::string arg      = argv[2];
    uint16_t    port     = DEFAULT_PORT;
    std::string out_path;
    bool        use_tcp  = false;

    for (int i = 3; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (flag == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (flag == "--tcp") {
            use_tcp = true;
        } else {
            std::cerr << "\nUnknown flag: " << flag << "\n";
            usage();
            return 1;
        }
    }

    try {
        if (cmd == "push") {
            if (use_tcp) {
                SocketFd listen_fd = tcp_listen(port);
                std::cout << "[main] listening on port " << port << " (TCP)\n";
                FileMeta file_meta = precompute_meta(arg);
                SocketFd file_fd{::open(arg.c_str(), O_RDONLY)};
                if (file_fd.get() < 0) {
                    throw std::runtime_error("failed to open file: " +
                                             arg + " - " + std::strerror(errno));
                }
                auto conn = tcp_accept(listen_fd.get());
                std::cout << "[main] TCP connection established\n";
                run_push(*conn, *conn, std::move(file_fd), std::move(file_meta));
            } else {
                SocketFd listen_fd = tcp_listen(port);
                std::cout << "[main] TCP listening on port " << port << "\n";
                FileMeta file_meta = precompute_meta(arg);
                SocketFd file_fd{::open(arg.c_str(), O_RDONLY)};
                if (file_fd.get() < 0) {
                    throw std::runtime_error("[main] failed to open file: " +
                                             arg + " - " + std::strerror(errno));
                }
                auto tcp_conn = tcp_accept(listen_fd.get());
                std::cout << "[main] TCP control established\n";
                // bind UDP then signal pull — pull connects after ready byte
                SocketFd udp_fd = udp_data_server_bind(
                    static_cast<uint16_t>(port + UDP_DATA_PORT_OFFSET));
                std::cout << "[main] UDP bound on port "
                          << port + UDP_DATA_PORT_OFFSET << "\n";
                uint8_t udp_ready = 1;
                send_exact(*tcp_conn, &udp_ready, 1);
                // block until pull sends probe
                auto udp_conn = udp_data_server_connect(std::move(udp_fd), *tcp_conn);
                std::cout << "[main] UDP peer locked\n";
                run_push(*tcp_conn, *udp_conn, std::move(file_fd), std::move(file_meta));
            }

        } else if (cmd == "pull") {
            if (use_tcp) {
                auto conn = tcp_connect(arg, port);
                std::cout << "[main] TCP connected to " << arg << ":" << port << "\n";
                run_pull(*conn, *conn, out_path);
            } else {
                auto tcp_conn = tcp_connect(arg, port);
                std::cout << "[main] TCP connected to " << arg << ":" << port << "\n";
                // recv ready — push has bound UDP
                uint8_t udp_ready{};
                recv_exact(*tcp_conn, &udp_ready, 1);
                // connect to push's UDP — sends probe, unblocks udp_wait_peer
                auto udp_conn = udp_data_client_connect(arg,
                    static_cast<uint16_t>(port + UDP_DATA_PORT_OFFSET), *tcp_conn);
                std::cout << "[main] UDP connected to " << arg << ":"
                          << port + UDP_DATA_PORT_OFFSET << "\n";
                run_pull(*tcp_conn, *udp_conn, out_path);
            }

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