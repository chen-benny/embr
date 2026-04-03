//
// Created by benny on 3/14/26.
//

#pragma once
#include "core/protocol.hpp"
#include "../transport/transport.hpp"
#include "../util/socket_fd.hpp"
#include <string>
#include <cstdint>

// Share side
FileMeta precompute_meta(const std::string& filepath);
void run_push(Transport& transport, SocketFd file_fd, FileMeta meta);