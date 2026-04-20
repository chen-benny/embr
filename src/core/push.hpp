//
// Created by benny on 3/14/26.
//

#pragma once
#include "core/protocol.hpp"
#include "transport/transport.hpp"
#include "util/socket_fd.hpp"
#include <string>
#include <cstdint>

// Phase 1: mmap entire file, compute SHA256 per chunk, return FileMeta with chunk_hashes
FileMeta precompute_meta(const std::string& filepath);

// Phase 2: serve file to peer over transport
// request-driven: recv CHUNK_REQ, send CHUNK_HDR + senf_file; break on COMPLETE
void run_push(Transport& t, SocketFd file_fd, FileMeta meta);