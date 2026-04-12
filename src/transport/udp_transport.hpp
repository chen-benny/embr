//
// Created by benny on 4/4/26.
//

#pragma once
#include "transport.hpp"
#include "util/constants.hpp"
#include "util/io_uring_ctx.hpp"
#include "util/socket_fd.hpp"
#include <cstddef>
#include <cstdint>
#include <bitset>
#include <memory>
#include <string>

// Fragment wire format 10 bytes: [chunk_index:u32 BE][frag_index:u32 BE][frag_len:u16 BE]
// Control plane is NOT used on UdpTransport: all control messages go over TcpTransport
// Data Plane: io_uring registered buffers, direct-to-disk
//   send_file: READ_FIXED chunk -> fragment. double buffer pipeline
//   recv_file: RECV + WRITE_FIXED direct to disk
// Non-copyable, constructed only via udp_connect() / udp_wait_peer() factories
class UdpTransport final : public Transport {
public:
    ~UdpTransport() override = default;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Control plane: unused, retain for Transport interface compliance
    ssize_t send(const uint8_t* buf, size_t len) override;
    ssize_t recv(uint8_t* buf, size_t len) override;

    // Data plane
    // send_file: read entire file via READ_FIXED, fragments into UDP datagrams
    // double buffer pipeline: read chunk N+1 overlaps send chunk N
    void send_file(int file_fd, uint64_t offset, size_t len) override;

    // recv_file: receives fragments via RECV, writes directly to disk
    // WRITE_FIXED with exact file offset, no reassembly
    void recv_file(int file_fd, uint64_t offset, size_t len) override;

private:
    explicit UdpTransport(SocketFd fd, Transport& tcp);

    // send_file internals
    // Submit a READ_FIXED SQE (DMA in background)
    void submit_read(size_t buf_idx, int file_fd, uint64_t offset, size_t len);
    // Wait for prev submitted READ_FIXED CQE
    void wait_read();
    // Fragment chunk_buf[buf_idx] into datagrams
    //   iov[0]: 10-byte fragment header (stack-allocated, CPU writes)
    //   iov[1]: 1390-byte data slice into (registered buffer, DMA)
    void send_chunk(size_t buf_idx, uint32_t chunk_index, size_t len);

    // recv_file internals
    // Submit a RECV SQE for frag_buf[frag_buf_idx]
    void submit_recv(size_t frag_buf_idx);

    SocketFd fd_; // connected UDP socket
    IoUringCtx ring_; // value member, owns io_uring ring + regi-bufs, lifetime == UdpTransport
    Transport& tcp_; // TCP control channel: cold start sync + per-chunk ACK

    // READ CQE stack - send_chunk drain loop may reap TAG_READ CQE before wait_read()
    // single slot for v0.4 - one READ in flight at a time
    bool read_completed_{false};
    int32_t read_result_{0};

    friend std::unique_ptr<Transport> udp_data_client_connect(const std::string& host,
                                                              uint16_t port,
                                                              Transport& tcp);
    friend std::unique_ptr<Transport> udp_data_server_connect(SocketFd fd, Transport& tcp);
};
