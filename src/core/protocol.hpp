//
// Created by benny on 3/11/26.
//

#pragma once

#include "hash.hpp"
#include "transport/transport.hpp"
#include "util/constants.hpp"
#include "util/io.hpp"
#include <cstring>
#include <cerrno>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

inline constexpr uint8_t PROTOCOL_VERSION = 0x02; // v0.3: chunk_hashes in FILE_META, ChunkHdr drop hash
inline constexpr size_t HEADER_SIZE = 6; // version(1) + type(1) + payload_len(4)
inline constexpr size_t FILE_SIZE_BYTES = sizeof(uint64_t); // 8 - [file_size:u64 BE]
inline constexpr size_t LEN_PREFIX_BYTES = sizeof(uint32_t); // 4 - [filename_len:u32 BE]
inline constexpr uint32_t MAX_PAYLOAD_SIZE = 64 * 1024 * 1024; // 64MB, guard against malformed headers

enum class MsgType : uint8_t {
    INVALID = 0x00, // sentinel for zero-value init
    HANDSHAKE = 0x01, // v0.2: HANDSHAKE introduced for tracker + token resolution
    FILE_META = 0x02, // v0.1: connections start directly with FILE_META
    CHUNK_REQ = 0x03, // v0.5: used in request-driven chunk communication
    CHUNK_HDR = 0x04,
    RESERVED = 0x05, // was RESUME, eliminated by request-driven design in v0.5
    COMPLETE = 0x06,
    ERROR = 0x07, // Error: sender emits reason string, both sides close connection
    CANCEL = 0x08, // CANCEL: receiver requests abort, sender stops and closes
};

// Wire Format
// all multi-byte fields are big-endian (network byte order)
// Header: [version:u8][type:u8][payload_len:u32 BE]
// FILE_META payload: [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
// HandshakePayload [token_len:u32 BE][token:utf8]
// ChunkReq: [chunk_index:u32 BE]
// ChunkHdr: [chunk_index:u32 BE]
// COMPLETE payload: empty
// ERROR payload: [reason:utf8] (length = payload_len)
struct Header {
    uint8_t version;
    MsgType type;
    uint32_t payload_len; // host order here, big-endian on wire
};

// Buffer
// move-only, supports both owned and pool-handle paths
// v0.1 heap: Buffer(n) - owned = make_unique<uint8_t[]>(n), release = nullptr
// v0.3 mmap: Buffer(ptr, n, [n](uint8_t* p){ munmap(p, n); }
// v0.4 io_uring registered pool: Buffer(ptr, n, [&pool](uint8_t* p) { pool.release(p); })
// std::function as type-erased release callback unifies three memory ownership models
struct Buffer {
    std::unique_ptr<uint8_t[]> owned; // on-demand allocation path -> heap
    uint8_t* ptr{}; // pool handle path (io_uring)
    size_t size{};
    std::function<void(uint8_t*)> release;

    Buffer() = default;

    // heap path - owned handles cleanup
    explicit Buffer(size_t n)
        : owned(std::make_unique<uint8_t[]>(n)), size(n) {}

    // mmap / pool path - release callback handles cleanup
    Buffer(uint8_t* p, size_t n, std::function<void(uint8_t*)> rel)
        : ptr(p), size(n), release(std::move(rel)) {}

    ~Buffer() {
        if (!owned && ptr && release) { release(ptr); }
    }

    // disable copy
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // enable move
    Buffer(Buffer&& other) noexcept
        : owned (std::move(other.owned))
        , ptr (std::exchange(other.ptr, nullptr))
        , size (std::exchange(other.size, 0))
        , release (std::exchange(other.release,nullptr)) {}

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            if (!owned && ptr && release) { release(ptr); }
            owned = std::move(other.owned);
            ptr = std::exchange(other.ptr, nullptr);
            size = std::exchange(other.size, 0);
            release = std::exchange(other.release,nullptr);
        }
        return *this;
    }

    uint8_t* get() { return owned ? owned.get() : ptr; }
    const uint8_t* get() const { return owned ? owned.get() : ptr; }
    bool valid() const { return get() != nullptr && size > 0; } // avoid: COMPLETE msg type is empty() == true
};

// Message
// move only, control plane only
// Data plane (v0.1): raw file bytes sent via send_exact, bypassing Message
// v0.2+: raw file bytes send as Message, bypass removed
struct Message {
    MsgType type{}; // zero-inited - 0x00 is INVALID MsgType
    Buffer payload;

    Message() = default;
    Message(MsgType t, Buffer payload) : type(t), payload(std::move(payload)) {}

    // disable copy
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    // enable move
    Message(Message&&) = default;
    Message& operator=(Message&&) = default;
};

// Typed payload structs, in-memory only
struct FileMeta {
    std::string file_name;
    uint64_t file_size{}; // bytes, big-endian on wire
    uint32_t chunk_size{};
    uint32_t chunk_count{};
    std::vector<std::array<uint8_t, HASH_SIZE>> chunk_hashes{}; // pre-computed, one per chunk
};

struct HandshakePayload {
    std::string token; // v0.2: empty, v0.6: real token
};

struct ChunkReq {
    uint32_t chunk_index{};
};

struct ChunkHdr {
    uint32_t chunk_index{};
};

// Public APIs
void send_msg(Transport& t, Message&& msg); // move only
Message recv_msg(Transport& t);

// Builders
Message make_filemeta(FileMeta file_meta);
Message make_complete();
Message make_error(std::string reason);
Message make_handshake(HandshakePayload p);
Message make_chunk_req(ChunkReq req);
Message make_chunk_hdr(ChunkHdr chunk_hdr); // header only, raw bytes separate

// Parsers
FileMeta parse_filemeta(const Message& msg);
std::string parse_error(const Message& msg);
HandshakePayload parse_handshake(const Message& msg);
ChunkReq parse_chunk_req(const Message& msg);
ChunkHdr parse_chunk_hdr(const Message& msg);