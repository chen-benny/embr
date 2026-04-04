//
// Created by benny on 3/23/26.
//

#pragma once
#include <cstddef>

namespace embr {

// Chunk
inline constexpr size_t READ_BUF_SIZE = 1024 * 1024; // 1MB for TCP-path fallback
inline constexpr size_t CHUNK_SIZE = 16 * 1024 * 1024; // 16MB

// UDP / io_uring
// UDP fragment
inline constexpr size_t UDP_MTU = 1400;
inline constexpr size_t FRAG_HDR_SIZE = 10;
inline constexpr size_t UDP_PAYLOAD_SIZE = UDP_MTU - FRAG_HDR_SIZE; // 1390
inline constexpr size_t MAX_FRAG_PER_CHUNK =
    (CHUNK_SIZE + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE; // 12,078
// UDP reliability
inline constexpr uint32_t UDP_ACK_TIMEOUT_MS = 50; // wait for chunk ack
inline constexpr int UDP_CHUNK_MAX_RETRIES = 10; // max chunk resend
inline constexpr int UDP_HELLO_TIMEOUT_S = 30; // udp_bind wait
inline constexpr int UDP_HELLO_MAX_RETRIES = 5; // udp_connect resends
// io_uring buffer poll
inline constexpr size_t UDP_CHUNK_BUFS = 2; // 2 * 16MB, double buffer, read/send pipeline
inline constexpr size_t UDP_FRAG_BUF = 64; // 64 * 1400B, recv fragment poll

}