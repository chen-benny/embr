//
// Created by benny on 4/5/26.
//

#include "core/protocol.hpp"
#include "util/constants.hpp"
#include "util/socket_fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "udp_data_server.hpp"
#include "udp_transport.hpp"

SocketFd udp_data_server_bind(uint16_t port) {
    int raw_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw_fd < 0) {
        throw std::runtime_error("udp_data_server_bind: socket() failed: " +
                                 std::string(std::strerror(errno)));
    }
    SocketFd fd{raw_fd};

    int opt = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        throw std::runtime_error("udp_data_server_bind: bind() failed: " +
                                 std::string(std::strerror(errno)));
    }

    return fd;
}

std::unique_ptr<Transport> udp_data_server_connect(SocketFd fd, Transport& tcp) {
    // block until probe arrives, learn peer addr, connect and lock
    uint8_t probe{};
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    ssize_t n = ::recvfrom(fd.get(), &probe, sizeof(probe), 0,
                             reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n <= 0) {
        throw std::runtime_error("udp_data_server_connect: recvfrom() failed: " +
                                 std::string(std::strerror(errno)));
    }

    // lock peer address
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&peer), peer_len) < 0) {
        throw std::runtime_error("udp_data_server_connect: connect() failed: " +
                                 std::string(std::strerror(errno)));
    }

    return std::unique_ptr<Transport>(new UdpTransport(std::move(fd), tcp));
}
