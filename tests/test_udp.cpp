//
// Created by benny on 4/11/26.
//
//
// Created by benny on 3/29/26.
//

#include <gtest/gtest.h>
#include "transport/tcp_client.hpp"     // tcp_connect
#include "transport/tcp_server.hpp"     // tcp_listen, tcp_accept
#include "transport/udp_data_server.hpp"       // udp_data_server_bind, udp_data_server_connect
#include "transport/udp_data_client.hpp"    // udp_data_client_connect
#include "util/io.hpp"                  // send_exact, recv_exact
#include "util/socket_fd.hpp"           // SocketFd
#include <arpa/inet.h>                  // htons, ntohs, getsockname, sockaddr_in
#include <fcntl.h>                      // ::open, O_RDWR, O_CREAT, O_TRUNC, O_RDONLY
#include <sys/mman.h>                   // ::mmap, ::munmap, PROT_READ, MAP_SHARED, MAP_FAILED
#include <unistd.h>                     // ::ftruncate, ::pread, ::write, ::unlink
#include <cstring>                      // std::memcmp
#include <future>                       // std::promise, std::future
#include <memory>                       // std::unique_ptr
#include <thread>                       // std::jthread
#include <vector>                       // std::vector

// ── Helpers ───────────────────────────────────────────────────────────────────

struct ListenSocket {
    int      fd;
    uint16_t port;
};

static ListenSocket make_listen_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { throw std::runtime_error("socket() failed"); }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); throw std::runtime_error("bind() failed");
    }
    if (::listen(fd, 1) < 0) {
        ::close(fd); throw std::runtime_error("listen() failed");
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return {fd, ntohs(addr.sin_port)};
}

struct TempFile {
    int         fd{-1};
    std::string path;
    TempFile() {
        char tmpl[] = "/tmp/embr_udp_test_XXXXXX";
        fd = ::mkstemp(tmpl);
        if (fd < 0) { throw std::runtime_error("mkstemp() failed"); }
        path = tmpl;
    }
    ~TempFile() {
        if (fd >= 0) { ::close(fd); }
        if (!path.empty()) { ::unlink(path.c_str()); }
    }
    TempFile(const TempFile&) = delete;
    TempFile(TempFile&&)      = delete;
};

static void fill_pattern(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(i & 0xFF);
    }
}

// ── Test fixture ──────────────────────────────────────────────────────────────
// Each test sets up:
//   - one TCP connection pair (for ACK channel)
//   - one UDP connection pair (for data plane)
// Push side runs in a jthread, pull side runs in the test body.

struct UdpTransportTest : ::testing::Test {
    // TCP ports picked by OS
    static constexpr uint16_t UDP_PORT = 19001;  // fixed for simplicity

    void run_transfer(size_t file_size) {
        // ── write source file ─────────────────────────────────────────────────
        TempFile src;
        std::vector<uint8_t> pattern(file_size);
        fill_pattern(pattern.data(), file_size);
        ::write(src.fd, pattern.data(), file_size);

        // ── dest file ─────────────────────────────────────────────────────────
        TempFile dst;
        ::ftruncate(dst.fd, static_cast<off_t>(file_size));

        // ── TCP listen ────────────────────────────────────────────────────────
        auto [tcp_listen_fd, tcp_port] = make_listen_socket();

        // ── UDP bind ready signal ─────────────────────────────────────────────
        std::promise<void> udp_bound;
        std::future<void>  udp_bound_fut = udp_bound.get_future();

        // ── push side (server) ────────────────────────────────────────────────
        std::jthread push_thread([&, tcp_port]() {
            // TCP accept
            auto tcp_conn = tcp_accept(tcp_listen_fd);

            // UDP bind + signal pull
            SocketFd udp_fd = udp_data_server_bind(UDP_PORT);
            udp_bound.set_value();   // tell pull: UDP port ready

            auto udp_conn = udp_data_server_connect(std::move(udp_fd), *tcp_conn);

            // send file
            udp_conn->send_file(src.fd, 0, file_size);
        });

        // ── pull side (client) ────────────────────────────────────────────────
        // TCP connect
        auto tcp_conn = tcp_connect("127.0.0.1", tcp_port);

        // wait for UDP bind
        udp_bound_fut.wait();

        auto udp_conn = udp_data_client_connect("127.0.0.1", UDP_PORT, *tcp_conn);

        // recv file
        udp_conn->recv_file(dst.fd, 0, file_size);

        push_thread.join();
        ::close(tcp_listen_fd);

        // ── verify ────────────────────────────────────────────────────────────
        std::vector<uint8_t> src_buf(file_size), dst_buf(file_size);
        ::pread(src.fd, src_buf.data(), file_size, 0);
        ::pread(dst.fd, dst_buf.data(), file_size, 0);
        EXPECT_EQ(std::memcmp(src_buf.data(), dst_buf.data(), file_size), 0);
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(UdpTransportTest, SendRecvFile_Aligned) {
    run_transfer(64 * 1024);   // 64KB — single chunk, aligned
}

TEST_F(UdpTransportTest, SendRecvFile_Unaligned) {
    run_transfer(65537);   // not power of two — exercises last-fragment edge
}

TEST_F(UdpTransportTest, SendRecvFile_MultiChunk) {
    run_transfer(3 * 1024 * 1024);   // 3MB — 3 chunks, exercises ACK loop
}
