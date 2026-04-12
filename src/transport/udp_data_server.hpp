//
// Created by benny on 4/5/26.
//

#pragma once
#include "transport.hpp"
#include "udp_transport.hpp"
#include "util/socket_fd.hpp"
#include <memory>
#include <cstdint>

// Binds UDP socket on port, return fd, non-blocking
SocketFd udp_data_server_bind(uint16_t port);

// Block on recvfrom probe, connect to peer and return UdpTransport
std::unique_ptr<Transport> udp_data_server_connect(SocketFd fd, Transport& tcp);