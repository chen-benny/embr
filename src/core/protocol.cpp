//
// Created by benny on 3/13/26.
//

#include "protocol.hpp"
#include <arpa/inet.h>

namespace {

void put_u32(Buffer& buf, size_t& pos, uint32_t val) {
    uint32_t net = htonl(val);
    std::memcpy(buf.get() + pos, &net, sizeof(net));
    pos += sizeof(net);
}

void put_u64(Buffer& buf, size_t& pos, uint64_t val) {
    // big-endian: high word first (MSB first)
    put_u32(buf, pos, static_cast<uint32_t>(val >> 32));
    put_u32(buf, pos, static_cast<uint32_t>(val));
}

void put_str(Buffer& buf, size_t& pos, const std::string& str) {
    put_u32(buf, pos, static_cast<uint32_t>(str.size())); // length prefix
    std::memcpy(buf.get() + pos, str.data(), str.size()); // raw UTF-8, no null terminator
    pos += str.size();
}

void put_bytes(Buffer& buf, size_t& pos, const uint8_t* data, size_t len) {
    std::memcpy(buf.get() + pos, data, len);
    pos += len;
}

struct Reader {
    const uint8_t* data;
    size_t pos;
    size_t len;

    // Bound checks before read, throws if truncated/malformed payload
    void ensure(size_t n) const {
        if (pos + n > len) { throw std::runtime_error("protocol: truncated payload"); }
    }

    uint32_t get_u32() {
        ensure(sizeof(uint32_t)); // de-serialization primitive
        uint32_t net{};
        std::memcpy(&net, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return ntohl(net);
    }

    uint64_t get_u64() {
        uint64_t hi = get_u32();
        uint64_t lo = get_u32();
        return (hi << 32) | lo;
    }

    std::string get_str() {
        uint32_t str_len = get_u32();
        ensure(str_len);
        std::string str(reinterpret_cast<const char*>(data + pos), str_len);
        pos += str_len;
        return str;
    }

    template<size_t N>
    std::array<uint8_t, N> get_bytes() {
        ensure(N);
        std::array<uint8_t, N> out{};
        std::memcpy(out.data(), data + pos, N);
        pos += N;
        return out;
    }
};

}

void send_msg(Transport& t, Message&& msg) {
    uint8_t hdr[HEADER_SIZE];
    hdr[0] = PROTOCOL_VERSION;
    hdr[1] = static_cast<uint8_t>(msg.type);
    uint32_t net_len = htonl(static_cast<uint32_t>(msg.payload.size));
    std::memcpy(hdr + 2, &net_len, sizeof(net_len));

    send_exact(t, hdr, HEADER_SIZE);
    if (msg.payload.size > 0) { send_exact(t, msg.payload.get(), msg.payload.size); }
}

Message recv_msg(Transport& t) {
    uint8_t hdr[HEADER_SIZE];
    recv_exact(t, hdr, HEADER_SIZE);

    if (hdr[0] != PROTOCOL_VERSION) {
        throw std::runtime_error("recv_msg: unsupported protocol version " + std::to_string(hdr[0]));
    }

    // validate MsgType range
    if (hdr[1] < static_cast<uint8_t>(MsgType::HANDSHAKE) || hdr[1] > static_cast<uint8_t>(MsgType::CANCEL)) {
        throw std::runtime_error("recv_msg: unknown message type " + std::to_string(hdr[1]));
    }

    Message msg;
    msg.type = static_cast<MsgType>(hdr[1]);

    uint32_t net_len{};
    std::memcpy(&net_len, hdr + 2, sizeof(net_len));
    uint32_t payload_len = ntohl(net_len);

    // guard against malformed headers
    if (payload_len > MAX_PAYLOAD_SIZE) {
        throw std::runtime_error("recv_msg: payload_len exceeds MAX_PAYLOAD_SIZE (" +
                                 std::to_string(payload_len) + "bytes)");
    }

    if (payload_len > 0) {
        msg.payload = Buffer(payload_len);
        recv_exact(t, msg.payload.get(), payload_len);
    }

    return msg;
}

Message make_filemeta(FileMeta file_meta) {
    if (file_meta.chunk_hashes.size() != file_meta.chunk_count) {
        throw std::runtime_error("make_filemeta: wrong number of chunk hashes - " +
            std::to_string(file_meta.chunk_hashes.size()) +
            " != chunk_count " + std::to_string(file_meta.chunk_count));
    }

    // wire layout: [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
    //              [chunk_size:u32][chunk_count:u32]
    //              [chunk_hashes[0]:32B]...[chunk_hashes[N-1]:32B]
    size_t payload_size = FILE_SIZE_BYTES + LEN_PREFIX_BYTES + file_meta.file_name.size()
                        + sizeof(uint32_t) + sizeof(uint32_t)
                        + file_meta.chunk_count * HASH_SIZE;
    Buffer buf(payload_size);
    size_t pos = 0;
    put_u64(buf, pos, file_meta.file_size);
    put_str(buf, pos, file_meta.file_name);
    put_u32(buf, pos, file_meta.chunk_size);
    put_u32(buf, pos, file_meta.chunk_count);
    for (const auto& hash : file_meta.chunk_hashes) {
        put_bytes(buf, pos, hash.data(), sizeof(hash));
    }
    return Message{MsgType::FILE_META, std::move(buf)};
}

Message make_complete() {
    return Message{MsgType::COMPLETE, Buffer{}};
}

Message make_error(std::string reason) {
    // ERROR payload: raw UTF-8 bytes, length implied by payload_len in header, no length prefix
    Buffer buf(reason.size());
    std::memcpy(buf.get(), reason.data(), reason.size());
    return Message{MsgType::ERROR, std::move(buf)};
}

Message make_handshake(HandshakePayload p) {
    size_t payload_size = LEN_PREFIX_BYTES + p.token.size();
    Buffer buf(payload_size);
    size_t pos = 0;
    put_str(buf, pos, p.token);
    return Message{MsgType::HANDSHAKE, std::move(buf)};
}

Message make_chunk_req(ChunkReq req) {
    Buffer buf(sizeof(uint32_t));
    size_t pos = 0;
    put_u32(buf, pos, req.chunk_index);
    return Message{MsgType::CHUNK_REQ, std::move(buf)};
}

Message make_chunk_hdr(ChunkHdr chunk_hdr) {
    // header only: chunk_index
    size_t payload_size = sizeof(uint32_t);
    Buffer buf(payload_size);
    size_t pos = 0;
    put_u32(buf, pos, chunk_hdr.chunk_index);
    return Message{MsgType::CHUNK_HDR, std::move(buf)};
}

FileMeta parse_filemeta(const Message& msg) {
    if (msg.type != MsgType::FILE_META) { throw std::runtime_error("parse_filemeta: wrong type"); }
    Reader reader{msg.payload.get(), 0, msg.payload.size};
    FileMeta file_meta;
    file_meta.file_size = reader.get_u64();
    file_meta.file_name = reader.get_str();
    file_meta.chunk_size = reader.get_u32();
    file_meta.chunk_count = reader.get_u32();
    // bounds check before allocation - guard malicious or corrupt chunk_count e.g. 0XFFFFFFFF
    reader.ensure(static_cast<size_t>(file_meta.chunk_count) * HASH_SIZE);
    file_meta.chunk_hashes.resize(file_meta.chunk_count);
    for (auto& hash : file_meta.chunk_hashes) {
        hash = reader.get_bytes<HASH_SIZE>();
    }
    return file_meta;
}

std::string parse_error(const Message& msg) {
    if (msg.type != MsgType::ERROR) { throw std::runtime_error("parse_error: wrong type"); }
    return std::string(reinterpret_cast<const char*>(msg.payload.get()), msg.payload.size);
}

HandshakePayload parse_handshake(const Message& msg) {
    if (msg.type != MsgType::HANDSHAKE) { throw std::runtime_error("parse_handshake: wrong type"); }
    Reader r{msg.payload.get(), 0, msg.payload.size};
    return HandshakePayload{r.get_str()};
}

ChunkReq parse_chunk_req(const Message& msg) {
    if (msg.type != MsgType::CHUNK_REQ) { throw std::runtime_error("parse_chunk_req: wrong type"); }
    Reader r{msg.payload.get(), 0, msg.payload.size};
    return ChunkReq{r.get_u32()};
}

ChunkHdr parse_chunk_hdr(const Message& msg) {
    if (msg.type != MsgType::CHUNK_HDR) { throw std::runtime_error("parse_chunk_hdr: wrong type"); }
    Reader r{msg.payload.get(), 0, msg.payload.size};
    ChunkHdr chunk_hdr;
    chunk_hdr.chunk_index = r.get_u32();
    return chunk_hdr;
}
