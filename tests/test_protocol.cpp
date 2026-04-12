//
// Created by benny on 3/15/26.
//
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <cstring>
#include <vector>
#include "core/protocol.hpp"

// ── Mock transport — in-memory pipe, no sockets ───────────────────────────────

class MockTransport : public Transport {
    std::vector<uint8_t> buf_;
    size_t               read_pos_{};
public:
    ssize_t send(const uint8_t* buf, size_t len) override {
        buf_.insert(buf_.end(), buf, buf + len);
        return static_cast<ssize_t>(len);
    }
    ssize_t recv(uint8_t* buf, size_t len) override {
        size_t available = buf_.size() - read_pos_;
        if (available == 0) { return 0; }
        size_t to_copy = std::min(len, available);
        std::memcpy(buf, buf_.data() + read_pos_, to_copy);
        read_pos_ += to_copy;
        return static_cast<ssize_t>(to_copy);
    }
    void send_file(int /*file_fd*/, uint64_t /*offset*/, size_t /*len*/) override {
        throw std::runtime_error("MockTransport: send_file not supported");
    }
    void recv_file(int /*file_fd*/, uint64_t /*offset*/, size_t /*len*/) override {
        throw std::runtime_error("MockTransport: recv_file not supported");
    }
    void inject(const uint8_t* data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }
};

// ── FileMeta round-trip ───────────────────────────────────────────────────────

TEST(Protocol, FileMetaRoundTrip) {
    MockTransport t;
    constexpr uint32_t kChunkCount = 3;
    std::array<uint8_t, 32> hash0{}, hash1{}, hash2{};
    hash0.fill(0xAA); hash1.fill(0xBB); hash2.fill(0xCC);
    FileMeta original{
        .file_name     = "testfile.iso",
        .file_size    = 3ULL * 1024 * 1024 * 1024,
        .chunk_size   = static_cast<uint32_t>(CHUNK_SIZE),
        .chunk_count  = kChunkCount,
        .chunk_hashes = {hash0, hash1, hash2},
    };

    send_msg(t, make_filemeta(FileMeta{original}));
    FileMeta parsed = parse_filemeta(recv_msg(t));

    EXPECT_EQ(parsed.file_name,    original.file_name);
    EXPECT_EQ(parsed.file_size,   original.file_size);
    EXPECT_EQ(parsed.chunk_size,  original.chunk_size);
    EXPECT_EQ(parsed.chunk_count, original.chunk_count);
    ASSERT_EQ(parsed.chunk_hashes.size(), kChunkCount);
    EXPECT_EQ(parsed.chunk_hashes[0], hash0);
    EXPECT_EQ(parsed.chunk_hashes[1], hash1);
    EXPECT_EQ(parsed.chunk_hashes[2], hash2);
}

TEST(Protocol, FileMetaEmptyFilename) {
    MockTransport t;
    std::array<uint8_t, 32> hash{};
    hash.fill(0x01);
    FileMeta meta{
        .file_name     = "",
        .file_size    = 1024,
        .chunk_size   = 16,
        .chunk_count  = 1,
        .chunk_hashes = {hash},
    };
    send_msg(t, make_filemeta(FileMeta{meta}));
    FileMeta parsed = parse_filemeta(recv_msg(t));

    EXPECT_EQ(parsed.file_name,    "");
    EXPECT_EQ(parsed.file_size,   1024ULL);
    EXPECT_EQ(parsed.chunk_size,  16U);
    EXPECT_EQ(parsed.chunk_count, 1U);
    ASSERT_EQ(parsed.chunk_hashes.size(), 1U);
    EXPECT_EQ(parsed.chunk_hashes[0], hash);
}

TEST(Protocol, FileMetaHashCountMismatchThrows) {
    FileMeta meta{
        .file_name     = "bad.bin",
        .file_size    = 1024,
        .chunk_size   = 512,
        .chunk_count  = 3,
        .chunk_hashes = {},   // size=0, count=3 → mismatch
    };
    EXPECT_THROW(make_filemeta(std::move(meta)), std::runtime_error);
}

TEST(Protocol, ParseFileMetaMaliciousChunkCountThrows) {
    MockTransport t;
    constexpr uint32_t kFakeCount = 0x00FFFFFF;

    // payload: file_size(8) + file_name_len(4) + file_name(0) + chunk_size(4) + chunk_count(4)
    // = 20 bytes, but claims kFakeCount * 32 hashes follow — they don't
    uint8_t payload[20] = {};
    payload[7] = 0x01;                          // file_size = 1
    payload[13] = 0x02; payload[14] = 0x00;     // chunk_size = 512
    uint32_t net_count = htonl(kFakeCount);
    std::memcpy(payload + 16, &net_count, 4);   // chunk_count = kFakeCount

    uint8_t hdr[HEADER_SIZE];
    hdr[0] = PROTOCOL_VERSION;
    hdr[1] = static_cast<uint8_t>(MsgType::FILE_META);
    uint32_t net_plen = htonl(static_cast<uint32_t>(sizeof(payload)));
    std::memcpy(hdr + 2, &net_plen, 4);
    t.inject(hdr, HEADER_SIZE);
    t.inject(payload, sizeof(payload));

    EXPECT_THROW(parse_filemeta(recv_msg(t)), std::runtime_error);
}

// ── HANDSHAKE round-trip ──────────────────────────────────────────────────────

TEST(Protocol, HandshakeEmptyToken) {
    MockTransport t;
    send_msg(t, make_handshake(HandshakePayload{""}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::HANDSHAKE);
    HandshakePayload parsed = parse_handshake(received);
    EXPECT_EQ(parsed.token, "");
}

TEST(Protocol, HandshakeWithToken) {
    MockTransport t;
    send_msg(t, make_handshake(HandshakePayload{"Kf3xQ9mZ"}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::HANDSHAKE);
    HandshakePayload parsed = parse_handshake(received);
    EXPECT_EQ(parsed.token, "Kf3xQ9mZ");
}

// ── ChunkReq round-trip ───────────────────────────────────────────────────────

TEST(Protocol, ChunkReqRoundTrip) {
    MockTransport t;
    send_msg(t, make_chunk_req(ChunkReq{42}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::CHUNK_REQ);
    ChunkReq parsed = parse_chunk_req(received);
    EXPECT_EQ(parsed.chunk_index, 42U);
}

TEST(Protocol, ChunkReqZeroIndex) {
    MockTransport t;
    send_msg(t, make_chunk_req(ChunkReq{0}));
    ChunkReq parsed = parse_chunk_req(recv_msg(t));
    EXPECT_EQ(parsed.chunk_index, 0U);
}

// ── ChunkHdr round-trip ───────────────────────────────────────────────────────
// v0.3: ChunkHdr carries only chunk_index — hash pre-communicated in FILE_META

TEST(Protocol, ChunkHdrRoundTrip) {
    MockTransport t;
    send_msg(t, make_chunk_hdr(ChunkHdr{.chunk_index = 7}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::CHUNK_HDR);
    ChunkHdr parsed = parse_chunk_hdr(received);
    EXPECT_EQ(parsed.chunk_index, 7U);
}

TEST(Protocol, ChunkHdrPayloadIsFourBytes) {
    MockTransport t;
    send_msg(t, make_chunk_hdr(ChunkHdr{.chunk_index = 0}));
    Message received = recv_msg(t);
    EXPECT_EQ(received.payload.size, sizeof(uint32_t));
}

// ── COMPLETE ──────────────────────────────────────────────────────────────────

TEST(Protocol, CompleteHasNoPayload) {
    MockTransport t;
    send_msg(t, make_complete());
    Message received = recv_msg(t);

    EXPECT_EQ(received.type, MsgType::COMPLETE);
    EXPECT_FALSE(received.payload.valid());
}

// ── ERROR round-trip ──────────────────────────────────────────────────────────

TEST(Protocol, ErrorRoundTrip) {
    MockTransport t;
    send_msg(t, make_error(std::string{"disk full"}));
    Message received = recv_msg(t);

    ASSERT_EQ(received.type, MsgType::ERROR);
    EXPECT_EQ(parse_error(received), "disk full");
}

TEST(Protocol, ErrorEmptyReason) {
    MockTransport t;
    send_msg(t, make_error(std::string{""}));
    EXPECT_EQ(parse_error(recv_msg(t)), "");
}

// ── recv_msg error handling ───────────────────────────────────────────────────

TEST(Protocol, WrongVersionThrows) {
    MockTransport t;
    uint8_t hdr[HEADER_SIZE] = {0x99, static_cast<uint8_t>(MsgType::FILE_META), 0, 0, 0, 0};
    t.inject(hdr, HEADER_SIZE);
    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

TEST(Protocol, UnknownMsgTypeThrows) {
    MockTransport t;
    uint8_t hdr[HEADER_SIZE] = {PROTOCOL_VERSION, 0xFF, 0, 0, 0, 0};
    t.inject(hdr, HEADER_SIZE);
    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

TEST(Protocol, OversizedPayloadThrows) {
    MockTransport t;
    uint8_t hdr[HEADER_SIZE];
    hdr[0] = PROTOCOL_VERSION;
    hdr[1] = static_cast<uint8_t>(MsgType::FILE_META);
    uint32_t huge = htonl(MAX_PAYLOAD_SIZE + 1);
    std::memcpy(hdr + 2, &huge, sizeof(huge));
    t.inject(hdr, HEADER_SIZE);
    EXPECT_THROW(recv_msg(t), std::runtime_error);
}

// ── parse wrong type throws ───────────────────────────────────────────────────

TEST(Protocol, ParseFileMetaWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    EXPECT_THROW(parse_filemeta(recv_msg(t)), std::runtime_error);
}

TEST(Protocol, ParseErrorWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    EXPECT_THROW(parse_error(recv_msg(t)), std::runtime_error);
}

TEST(Protocol, ParseHandshakeWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    EXPECT_THROW(parse_handshake(recv_msg(t)), std::runtime_error);
}

TEST(Protocol, ParseChunkReqWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    EXPECT_THROW(parse_chunk_req(recv_msg(t)), std::runtime_error);
}

TEST(Protocol, ParseChunkHdrWrongTypeThrows) {
    MockTransport t;
    send_msg(t, make_complete());
    EXPECT_THROW(parse_chunk_hdr(recv_msg(t)), std::runtime_error);
}

// ── Buffer ────────────────────────────────────────────────────────────────────

TEST(Buffer, HeapPathValid) {
    Buffer buf(1024);
    EXPECT_TRUE(buf.valid());
    EXPECT_EQ(buf.size, 1024ULL);
    EXPECT_NE(buf.get(), nullptr);
}

TEST(Buffer, DefaultInvalid) {
    Buffer buf;
    EXPECT_FALSE(buf.valid());
}

TEST(Buffer, MoveTransfersOwnership) {
    Buffer a(1024);
    uint8_t* ptr = a.get();
    Buffer b = std::move(a);

    EXPECT_EQ(b.get(), ptr);
    EXPECT_FALSE(a.valid());
}

TEST(Buffer, MoveAssignment) {
    Buffer a(1024);
    Buffer b(512);
    uint8_t* ptr = a.get();
    b = std::move(a);

    EXPECT_EQ(b.get(), ptr);
    EXPECT_EQ(b.size, 1024ULL);
    EXPECT_FALSE(a.valid());
}

TEST(Buffer, ReleaseCallbackCalled) {
    bool released = false;
    {
        uint8_t raw[64];
        Buffer buf(raw, sizeof(raw), [&released](uint8_t*) { released = true; });
        EXPECT_TRUE(buf.valid());
    }
    EXPECT_TRUE(released);
}