//
// Created by benny on 3/15/26.
//

#include "pull.hpp"

#include "transport/udp_transport.hpp"
#include "util/socket_fd.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "chunk_manager.hpp"
#include "hash.hpp"
#include "protocol.hpp"

void run_pull(Transport& tcp, Transport& udp, const std::string& output_path) {
    // send HANDSHAKE
    send_msg(tcp, make_handshake(HandshakePayload{""}));

    // recv FILE_META
    Message meta_msg = recv_msg(tcp);
    if (meta_msg.type != MsgType::FILE_META) {
        throw std::runtime_error("run_pull: expected FILE_META");
    }
    FileMeta meta = parse_filemeta(meta_msg);
    std::cout << "[pull] file=" << meta.file_name
              << " size=" << meta.file_size
              << " chunks=" << meta.chunk_count << "\n";

    // open output file
    const std::string& out = output_path.empty() ? meta.file_name : output_path;
    int raw_fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (raw_fd < 0) {
        throw std::runtime_error("run_pull: failed to open file " + out +
                                 " - " + std::strerror(errno));
    }
    SocketFd fd{raw_fd};

    // pre-allocate file size: enables pwrite at arbitrary offsets
    if (::ftruncate(fd.get(), static_cast<off_t>(meta.file_size)) < 0) {
        throw std::runtime_error("run_pull: failed to truncate file - " +
                                 std::string(std::strerror(errno)));
    }

    // recv entire file - transport handles chunking
    // TCP path: mmap(MAP_SHARED) + recv_exact, 1 copy
    // UDP path: RECV_FIXED + WRITE_FIXED direct-to-disk, 0 copy
    udp.recv_file(fd.get(), 0, meta.file_size);

    // verify chunk hashes
    std::cout << "[pull] verifying chunks...\n";
    for (uint32_t chunk_idx = 0; chunk_idx < meta.chunk_count; ++chunk_idx) {
        uint64_t offset = static_cast<uint64_t>(chunk_idx) * CHUNK_SIZE;
        size_t chunk_len = static_cast<size_t>(
            std::min(static_cast<uint64_t>(meta.chunk_size), meta.file_size - offset));

        void* mapped = ::mmap(nullptr, chunk_len, PROT_READ, MAP_SHARED,
                              fd.get(), static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::runtime_error("run_pull: mmap for verify failed at chunk " +
                                     std::to_string(chunk_idx) + " - " + std::strerror(errno));
        }
        auto computed = sha256_buf(static_cast<const uint8_t*>(mapped), chunk_len);
        ::munmap(mapped, chunk_len);

        if (computed != meta.chunk_hashes[chunk_idx]) {
            throw std::runtime_error("run_pull: hash mismatch at chunk " +
                                     std::to_string(chunk_idx));
        }
        std::cout << "[pull] verified " << chunk_idx + 1 << "/" << meta.chunk_count
                  << "\r" << std::flush;
    }
    std::cout << "\n[pull] all chunks verified\n";

    // send COMPLETE
    send_msg(tcp, make_complete());
    std::cout << "[pull] transfer complete - saved to " << out << "\n";
}