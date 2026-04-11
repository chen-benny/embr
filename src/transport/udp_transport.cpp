//
// Created by benny on 4/4/26.
//

#include "udp_transport.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../core/protocol.hpp"
#include "../util/constants.hpp"
#include "../util/socket_fd.hpp"
#include <liburing.h>

namespace {

// SQE user_data encoding:
// [tag:8 bits][buf_idx:56 bits] packed into io_uring sqe->user_data (uint64_t)
//   tag: READ/SEND/RECV/WRITE CQEs in shared drain loops
//   buf_idx: identifies which reg-buf slot for CQE to refer to
enum class SqeTag : uint8_t {
    READ = 1, // READ_FIXED: disk -> chunk_buf
    SEND = 2, // sendmsg: chunk_buf slice -> NIC
    RECV = 3, // RECV_FIXED: NIC -> frag_buf
    WRITE = 4, // WRITE_FIXED: frag_buf -> disk
};

static constexpr int TAG_SHIFT = 56;
static constexpr uint64_t IDX_MASK = 0x00FFFFFFFFFFFFFF;

static uint64_t make_user_data(SqeTag tag, size_t idx) {
    return (static_cast<uint64_t>(tag) << TAG_SHIFT) | static_cast<uint64_t>(idx);
}
static SqeTag get_tag(uint64_t user_data) {
    return static_cast<SqeTag>(user_data >> TAG_SHIFT);
}
static size_t get_index(uint64_t user_data) {
    return static_cast<size_t>(user_data & IDX_MASK);
}

// Fragment header (10 bytes): [chunk_index:u32 BE][frag_index:u32 BE][frag_len:u16 BE]
struct FragHdr {
    uint32_t chunk_index;
    uint32_t frag_index;
    uint16_t frag_len;
};

static FragHdr parse_frag_hdr(const uint8_t* buf) {
    FragHdr hdr{};
    uint32_t chunk_index_be{};
    uint32_t frag_index_be{};
    uint16_t frag_len_be{};
    std::memcpy(&chunk_index_be,
                buf,
                sizeof(chunk_index_be));
    std::memcpy(&frag_index_be,
                buf + sizeof(chunk_index_be),
                sizeof(frag_index_be));
    std::memcpy(&frag_len_be,
                buf + sizeof(chunk_index_be) + sizeof(frag_index_be),
                sizeof(frag_len_be));
    hdr.chunk_index = ntohl(chunk_index_be);
    hdr.frag_index = ntohl(frag_index_be);
    hdr.frag_len = ntohs(frag_len_be);
    return hdr;
}

static void write_frag_hdr(uint8_t* buf,
                           uint32_t chunk_index,
                           uint32_t frag_index,
                           uint16_t frag_len) {
    uint32_t chunk_index_be = htonl(chunk_index);
    uint32_t frag_index_be = htonl(frag_index);
    uint16_t frag_len_be = htons(frag_len);
    std::memcpy(buf,
                &chunk_index_be, sizeof(chunk_index_be));
    std::memcpy(buf + sizeof(chunk_index_be),
                &frag_index_be, sizeof(frag_index_be));
    std::memcpy(buf + sizeof(chunk_index_be) + sizeof(frag_index_be),
                &frag_len_be, sizeof(frag_len_be));
}

// Pre-chunk recv state tracks fragment for one chunk
struct RecvState {
    int file_fd;
    uint64_t chunk_offset;
    uint32_t frag_count;
    std::bitset<MAX_FRAGS_PER_CHUNK> received{};
    size_t frag_remaining;
};

// Process one completed RECV_FIXED fragment:
//   parse 10 Byte header -> submit WRITE_FIXED at exact offset -> update bitset
//   returns true if WRITE SQE prepped, false if dup, NO call of io_uring_submit
static bool process_fragment(size_t frag_buf_idx,
                             int32_t bytes_received,
                             IoUringCtx& ring,
                             RecvState& state) {
    if (bytes_received < static_cast<int32_t>(FRAG_HDR_SIZE)) {
        throw std::runtime_error("process_fragment: datagram too short: " +
                                 std::to_string(bytes_received));
    }

    uint8_t* buf = ring.frag_buf(frag_buf_idx);
    FragHdr hdr = parse_frag_hdr(buf);

    if (hdr.frag_index >= state.frag_count) {
        throw std::runtime_error("process_fragment: frag_index out of range: " +
                                 std::to_string(hdr.frag_index));
    }

    if (state.received.test(hdr.frag_index)) {
        return false; // dup fragment - no WRITE_FIXED submitted
    }

    uint64_t file_offset = state.chunk_offset +
                           static_cast<uint64_t>(hdr.frag_index) * UDP_PAYLOAD_SIZE;

    io_uring_sqe* sqe = io_uring_get_sqe(ring.ring());
    if (!sqe) {
        throw std::runtime_error("process_fragment: io_uring_get_sqe failed (ring full)");
    }
    // WRITE_FIXED: frag_buf data portion -> disk at file_offset
    // registered buffers first, otherwise plain WRITE
    if (ring.has_registered_buffers()) {
        io_uring_prep_write_fixed(sqe,
                                   state.file_fd,
                                   buf + FRAG_HDR_SIZE,
                                   hdr.frag_len,
                                   static_cast<__u64>(file_offset),
                                   static_cast<int>(ring.ring_index_frag(frag_buf_idx)));
    } else {
        io_uring_prep_write(sqe,
                            state.file_fd,
                            buf + FRAG_HDR_SIZE,
                            hdr.frag_len,
                            static_cast<__u64>(file_offset));
    }
    io_uring_sqe_set_data64(sqe, make_user_data(SqeTag::WRITE, frag_buf_idx));
    // NO submit

    state.received.set(hdr.frag_index);
    --state.frag_remaining;
    return true;
}

}

// UdpTransport
UdpTransport::UdpTransport(SocketFd fd)
    : fd_(std::move(fd))
    , ring_(UDP_CHUNK_BUFS, CHUNK_SIZE, UDP_FRAG_BUFS, UDP_MTU)
{}

// Control plane (un-used)
ssize_t UdpTransport::send(const uint8_t* buf, size_t len) {
    return ::send(fd_.get(), buf, len, MSG_NOSIGNAL);
}

ssize_t UdpTransport::recv(uint8_t* buf, size_t len) {
    return ::recv(fd_.get(), buf, len, 0);
}

// Data plane
// senf_file internals
void UdpTransport::submit_read(size_t buf_idx, int file_fd, uint64_t offset, size_t len) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.ring());
    if (!sqe) {
        throw std::runtime_error("submit_read: io_uring_get_sqe failed (ring full)");
    }
    if (ring_.has_registered_buffers()) {
        io_uring_prep_read_fixed(sqe,
                                 file_fd,
                                 ring_.chunk_buf(buf_idx),
                                 static_cast<unsigned>(len),
                                 static_cast<__u64>(offset),
                                 static_cast<int>(ring_.ring_index_chunk(buf_idx)));
    } else {
        io_uring_prep_read(sqe,
                           file_fd,
                           ring_.chunk_buf(buf_idx),
                           static_cast<unsigned>(len),
                           static_cast<__u64>(offset));
    }
    io_uring_sqe_set_data64(sqe, make_user_data(SqeTag::READ, buf_idx));

    io_uring_submit(ring_.ring());
}

void UdpTransport::wait_read() {
    if (read_completed_) { // check stash - send_chunk drain may already reap this CQE
        read_completed_ = false;
        if (read_result_ < 0) {
            throw std::runtime_error("wait_read: READ_FIXED failed: " +
                                     std::string(std::strerror(-read_result_)));
        }
        return;
    }

    while (true) {
        io_uring_cqe* cqe = nullptr;
        if (io_uring_wait_cqe(ring_.ring(), &cqe) < 0) {
            throw std::runtime_error("wait_read: io_uring_wait_cqe failed");
        }
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        SqeTag tag = get_tag(ud);
        int32_t res = cqe->res;
        io_uring_cqe_seen(ring_.ring(), cqe);
        if (tag == SqeTag::READ) {
            if (res < 0) {
                throw std::runtime_error("wait_read: READ_FIXED failed: " +
                                         std::string(std::strerror(-res)));
            }
            return;
        }
        throw std::runtime_error("wait_read: unexpected CQE tag");
    }
}

void UdpTransport::send_chunk(size_t buf_idx, uint32_t chunk_index, size_t len) {
    uint32_t frag_count = static_cast<uint32_t>(
        (len + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE);
    uint8_t* chunk = ring_.chunk_buf(buf_idx);

    // pre-compute all headers: 755 * 10B = 7.5KB for seq write
    std::vector<std::array<uint8_t, FRAG_HDR_SIZE>> hdrs(frag_count);
    for (uint32_t i = 0; i < frag_count; ++i) {
        size_t frag_len = (i + 1 < frag_count)
                          ? UDP_PAYLOAD_SIZE
                          : len - static_cast<size_t>(i) * UDP_PAYLOAD_SIZE;
        write_frag_hdr(hdrs[i].data(), chunk_index, i, static_cast<uint16_t>(frag_len));
    }

    // submit sendmsg SQEs using reusable slot pool: 256 * 88B = 22KB fits L1 cache
    constexpr size_t QUEUE_DEPTH = 256;
    struct SendSlot {
        iovec iov[2];
        msghdr msg{};
    };
    std::array<SendSlot, QUEUE_DEPTH> slots{};

    uint32_t pending_frags = 0;

    for (uint32_t frag_idx = 0; frag_idx < frag_count; ++frag_idx) {
        size_t frag_len = (frag_idx + 1 < frag_count)
                          ? UDP_PAYLOAD_SIZE
                          : len - static_cast<size_t>(frag_idx) * UDP_PAYLOAD_SIZE;

        if (pending_frags >= QUEUE_DEPTH) { // wait for a free slot if ring is full
            io_uring_submit(ring_.ring());
            while (pending_frags >= QUEUE_DEPTH) {
                io_uring_cqe* cqe = nullptr;
                if (io_uring_wait_cqe(ring_.ring(), &cqe) < 0) {
                    throw std::runtime_error("send_chunk: io_uring_wait_cqe failed");
                }
                uint64_t ud = io_uring_cqe_get_data64(cqe);
                SqeTag tag = get_tag(ud);
                int32_t res = cqe->res;
                io_uring_cqe_seen(ring_.ring(), cqe);

                if (tag == SqeTag::READ) {
                    read_completed_ = true;
                    read_result_ = res;
                } else if (tag == SqeTag::SEND) {
                    if (res < 0) {
                        throw std::runtime_error("send_chunk: send_msg failed: " +
                                                 std::string(std::strerror(-res)));
                    }
                    --pending_frags;
                }
            }
        }

        io_uring_sqe* sqe = io_uring_get_sqe(ring_.ring());
        if (!sqe) {
            throw std::runtime_error("send_chunk: io_uring_get_sqe failed");
        }

        SendSlot& slot = slots[pending_frags % QUEUE_DEPTH];
        slot.iov[0] = {hdrs[frag_idx].data(), FRAG_HDR_SIZE};
        slot.iov[1] = {chunk + static_cast<size_t>(frag_idx) * UDP_PAYLOAD_SIZE, frag_len};
        slot.msg.msg_iov = slot.iov;
        slot.msg.msg_iovlen = 2;

        io_uring_prep_sendmsg(sqe, fd_.get(), &slot.msg, 0);
        io_uring_sqe_set_data64(sqe, make_user_data(SqeTag::SEND, frag_idx));
        ++pending_frags;
    }

    // drain remaining send CQEs
    io_uring_submit(ring_.ring());
    while (pending_frags > 0) {
        io_uring_cqe* cqe = nullptr;
        if (io_uring_wait_cqe(ring_.ring(), &cqe) < 0) {
            throw std::runtime_error("send_chunk: final drain failed");
        }
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        SqeTag tag = get_tag(ud);
        int32_t res = cqe->res;
        io_uring_cqe_seen(ring_.ring(), cqe);
        if (tag == SqeTag::READ) {
            read_completed_ = true;
            read_result_ = res;
        } else if (tag == SqeTag::SEND) {
            if (res < 0) {
                throw std::runtime_error("send_chunk: final drain failed: " +
                                         std::string(std::strerror(-res)));
            }
            --pending_frags;
        }
    }
}


// send_file: handles entire file
void UdpTransport::send_file(int file_fd, uint64_t offset, size_t len) {
    uint32_t chunk_count = static_cast<uint32_t>((len + CHUNK_SIZE - 1) / CHUNK_SIZE);

    // cold start: read chunk 0 into buf 0
    submit_read(0, file_fd, offset, std::min(len, CHUNK_SIZE));
    wait_read();

    for (uint32_t chunk_idx = 0; chunk_idx < chunk_count; ++chunk_idx) {
        size_t buf_idx = chunk_idx & 1;
        size_t chunk_len = std::min(
            len - static_cast<size_t>(chunk_idx) * CHUNK_SIZE, CHUNK_SIZE);

        // overlap: read n + 1, send n
        if (chunk_idx + 1 < chunk_count) {
            size_t next_chunk_len = std::min(
                len - static_cast<size_t>(chunk_idx + 1) * CHUNK_SIZE, CHUNK_SIZE);
            submit_read((chunk_idx + 1) & 1,
                        file_fd,
                        offset + static_cast<uint64_t>(chunk_idx + 1) * CHUNK_SIZE,
                        next_chunk_len);
        }
        send_chunk(buf_idx, chunk_idx, chunk_len);

        if (chunk_idx + 1 < chunk_count) {
            wait_read();
        }
    }
}

// recv_file internals
void UdpTransport::submit_recv(size_t frag_buf_idx) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.ring());
    if (!sqe) {
        throw std::runtime_error("submit_recv: io_uring_get_sqe failed (ring full)");
    }
    // use registered buf via IORING_RECVSEND_FIXED_BUF (kernel 6.0+)
    // NIC DMA -> pinned frag buf directly
    io_uring_prep_recv(sqe,
                       fd_.get(),
                       ring_.frag_buf(frag_buf_idx),
                       ring_.frag_buf_size(),
                       0);
    sqe->buf_index = static_cast<uint16_t>(ring_.ring_index_frag(frag_buf_idx));
    sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
    io_uring_sqe_set_data64(sqe, make_user_data(SqeTag::RECV, frag_buf_idx));
    // no submit here, callers batches
}

// recv_file: handle entire file
void UdpTransport::recv_file(int file_fd, uint64_t offset, size_t len) {
    uint32_t chunk_count = static_cast<uint32_t>((len + CHUNK_SIZE - 1) / CHUNK_SIZE);

    // pre-fill frag pool once for entire file transfer
    // RECV SQE lifetime spans all chunks, no per-chunk cancel/resubmit
    for (size_t i = 0; i < ring_.frag_buf_count(); ++i) {
        submit_recv(i);
    }
    io_uring_submit(ring_.ring());
    size_t pending_recvs = ring_.frag_buf_count();

    for (uint32_t chunk_idx = 0; chunk_idx < chunk_count; ++chunk_idx) {
        uint64_t chunk_offset = offset + static_cast<uint64_t>(chunk_idx) * CHUNK_SIZE;
        size_t chunk_len = std::min(len - static_cast<size_t>(chunk_idx) * CHUNK_SIZE, CHUNK_SIZE);
        uint32_t frag_count = static_cast<uint32_t>(
            (chunk_len + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE);

        RecvState state{
            .file_fd = file_fd,
            .chunk_offset = chunk_offset,
            .frag_count = frag_count,
            .frag_remaining = frag_count,
        };

        size_t pending_writes = 0;
        const size_t flush_threshold = ring_.frag_buf_count() / 2;

        // CQE loop: drive until all fragments written to disk
        while (state.frag_remaining > 0 || pending_writes > 0) {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe(ring_.ring(), &cqe) < 0) {
                throw std::runtime_error("recv_file: io_uring_wait_cqe failed");
            }
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            SqeTag tag = get_tag(ud);
            size_t buf_idx = get_index(ud);
            int32_t res = cqe->res;
            io_uring_cqe_seen(ring_.ring(), cqe);
            if (tag == SqeTag::RECV) {
                --pending_recvs;
                if (res <= 0) {
                    throw std::runtime_error("recv_file: RECV failed: " +
                                             std::string(std::strerror(-res)));
                }
                if (process_fragment(buf_idx, res, ring_, state)) {
                    ++pending_writes;
                    // flush at threshold or last frag recv
                    // guards un-submitted WRITE SQEs causing deadlock
                    if (pending_writes >= flush_threshold || state.frag_remaining == 0) {
                        io_uring_submit(ring_.ring());
                    }
                } else {
                    // duplicate frag, reclaim buffer immediately
                    submit_recv(buf_idx);
                    ++pending_recvs;
                    if (pending_recvs >= flush_threshold) {
                        io_uring_submit(ring_.ring());
                    }
                }

            } else if (tag == SqeTag::WRITE) {
                if (res < 0) {
                    throw std::runtime_error("recv_file: WRITE_FIXED failed: " +
                                             std::string(std::strerror(-res)));
                }
                --pending_writes;
                // WRITE complete, buf safe to reclaim next frag
                if (state.frag_remaining > 0) {
                    submit_recv(buf_idx);
                    ++pending_recvs;
                    if (pending_recvs >= flush_threshold) {
                        io_uring_submit(ring_.ring());
                    }
                }
            }
        }
    }
    // outstanding RECV SQEs cleaned up by IoUringCtx::~IoUringCtx -> io_uring_queue_exit
}