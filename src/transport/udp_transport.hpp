//
// Created by benny on 4/4/26.
//

#pragma once
#include "transport.hpp"
#include "../util/constants.hpp"
#include "../util/io_uring_ctx.hpp"
#include "../util/socket_fd.hpp"
#include <cstddef>
#include <cstdint>
#include <bitset>
#include <memory>
#include <string>

inline constexpr uint32_t UDP_HELLO = 0x454D4252; // "EMBR"
inline constexpr uint8_t UDP_ACK = 0xFF; // sentinel byte to distinguish from fragment datagrams

// Fragment wire format (10 bytes)
//   [chunk_index:u32 BE][frag_index:u32 BE][frag_len:u16 BE]
// ACK wire format (5 bytes, recv -> send)
//   [UDP_ACK:u8][chunk_index:u32 BE]
// HELLO wire format (5 bytes, internal)
//   [UDP_HELLO:u32 BE][version:u8=PROTOCOL_VERSION]
// Control plane: ::send/::recv - NO io_uring
// Data plane
//   send_file: double-buffer pipeline
//   recv_file: direct-to-disk pipeline
// Copy cost: both 0 copies for send/recv, CPU touches 10-byte fragment headers, datapath pure DMA
// Reliability: 1 chunk in flight at a time, 50ms ACK timeout, up to 10 retransmits
// Non-copyable, constructable only via udp_connect() / udp_bind() factories
class UdpTransport final : public Transport {
public:
    ~UdpTransport() override = default;

    UdpTransport(UdpTransport const &) = delete;
    UdpTransport& operator=(UdpTransport const &) = delete;

    // Control plane
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

    // Data plane
    void send_file(int file_fd, uint64_t offset, size_t len) override;
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

private:
    explicit UdpTransport(SocketFd fd);

    // send_file internals
    // Submit a READ_FIXED SQE to read file_fd at [offset, offset+len) into chunk_buf[buf_idx]
    void submit_read(size_t buf_idx, int file_fd, uint64_t offset, size_t len);
    // Wait for prev submitted READ_FIXED CQE, only one READ in flight at a time (v0.4)
    void wait_read();
    // Fragment chunk_buf[buf_idx] into datagrams
    //   iov[0]: 10-byte fragment header (stack-allocated, CPU writes)
    //   iov[1]: 1390-byte data slice into reg-chunk-buf (DMA, CPU free)
    void send_chunk(size_t buf_idx, uint32_t chunk_index, size_t len);
    // Poll fd for 5-byte ACK datagrams matching chunk_index
    bool wait_ack(uint32_t chunk_index);

    // recv_file internals
    // Submit UDP_FRAG_BUFS RECV_FIXED SQEs
    void submit_recv(size_t frag_buf_idx);

    SocketFd fd_; // connected UDP socket
    IoUringCtx ring_; // value member, owns io_uring ring + regi-bufs, lifetime == UdpTransport

    // READ CQE stack - send_chunk drain loop may reap TAG_READ CQE before wait_read()
    // single slot for v0.4 - one READ in flight at a time
    bool read_completed_{false};
    int32_t read_result_{0};

    friend std::unique_ptr<Transport> udp_connect(const std::string& host, uint16_t port);
    friend std::unique_ptr<Transport> udp_bind(uint16_t port);
};
