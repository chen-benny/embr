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

#include "udp_data_client.hpp"
#include "udp_transport.hpp"

std::unique_ptr<Transport> udp_data_client_connect(const std::string& host,
                                                   uint16_t port, Transport& tcp) {
    int raw_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw_fd < 0) {
        throw std::runtime_error("udp_data_client_connect: socket() failed: " +
                                 std::string(std::strerror(errno)));
    }
    SocketFd fd{raw_fd};

    int rcvbuf = 16 * 1024 * 1024; // 16MB, handles burst of 1MB chunks
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // connect - locks peer addr
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr.s_addr) != 1) {
        throw std::runtime_error("udp_data_client_connect: invalid address: " + host);
    }
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("udp_data_client_connect: connect() failed: " +
                                 std::string(std::strerror(errno)));
    }
    // send 1-byte probe to udp_wait_peer
    uint8_t probe = 0;
    if (::send(fd.get(), &probe, sizeof(probe), 0) < 0) {
        throw std::runtime_error("udp_data_client_connect: probe send() failed: " +
                                 std::string(std::strerror(errno)));
    }

    return std::unique_ptr<Transport>(new UdpTransport(std::move(fd), tcp));
}