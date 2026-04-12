//
// Created by benny on 4/5/26.
//

#pragma once
#include "transport.hpp"
#include "udp_transport.hpp"
#include <memory>
#include <string>
#include <cstdint>

// Connect UDP socket to host:port, sends 1-byte probe
std::unique_ptr<Transport> udp_data_client_connect(const std::string& host,
                                                   uint16_t port, Transport& tcp);