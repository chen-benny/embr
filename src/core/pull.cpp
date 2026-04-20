//
// Created by benny on 3/15/26.
//

#include "chunk_manager.hpp"
#include "hash.hpp"
#include "protocol.hpp"
#include "partial_file.hpp"
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
#include <string>
#include <vector>
#include <optional>

void run_pull(Transport& t, const std::string& output_path) {
    // send HANDSHAKE
    send_msg(t, make_handshake(HandshakePayload{""}));

    // recv FILE_META
    Message meta_msg = recv_msg(t);
    if (meta_msg.type != MsgType::FILE_META) {
        throw std::runtime_error("run_pull: expected FILE_META");
    }
    FileMeta file_meta = parse_filemeta(meta_msg);
    std::cout << "[pull] file=" << file_meta.file_name
              << " size=" << file_meta.file_size
              << " chunks=" << file_meta.chunk_count << "\n";

    // open output file
    const std::string& out = output_path.empty() ? file_meta.file_name : output_path;

    // check for prior partial transfer
    std::optional<ChunkManager> cm_opt = PartialFile::load(
        out, file_meta.chunk_hashes[0], file_meta.chunk_count); // chunk_hashes[0] verify whole file
    ChunkManager cm = cm_opt.value_or(ChunkManager(file_meta.chunk_count));

    // open outputfile
    //   resume: file exist at correct size, no truncate
    //   fresh: create and pre-allocate
    int raw_fd = -1;
    if (cm_opt) {
        raw_fd = ::open(out.c_str(), O_RDWR);
        if (raw_fd < 0) { // output file gone while partial file existing, fallback to fresh
            cm = ChunkManager(file_meta.chunk_count);
            raw_fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0664);
        }
    } else {
       raw_fd = ::open(out.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0664);
    }
    if (raw_fd < 0) {
        throw std::runtime_error("run_pull: failed to open file " + out +
                                 " - " + std::strerror(errno));
    }
    SocketFd fd{raw_fd};

    if (!cm_opt) {
        // pre-allocate for fresh
        if (::ftruncate(fd.get(), static_cast<off_t>(file_meta.file_size)) < 0) {
            throw std::runtime_error("run_pull: failed to truncate file - " +
                                     std::string(std::strerror(errno)));
        }
    }

    // send CHUNK_REQ for needed chunks, then COMPLETE
    const std::vector<uint32_t> needed = cm.needed_chunks();
    std::cout << "[pull] requesting " << needed.size()
              << "/" << file_meta.chunk_count << " chunks\n";
    for (uint32_t chunk_index : needed) {
        send_msg(t, make_chunk_req(ChunkReq{chunk_index}));
    }
    send_msg(t, make_complete());

    for (uint32_t chunk_index : needed) {
        // recv CHUNK_HDR
        Message hdr_msg = recv_msg(t);
        if (hdr_msg.type != MsgType::CHUNK_HDR) {
            throw std::runtime_error("run_pull: expected CHUNK_HDR for chunk "  +
                                     std::to_string(chunk_index));
        }

        ChunkHdr hdr = parse_chunk_hdr(hdr_msg);
        if (hdr.chunk_index != chunk_index) {
            throw std::runtime_error("run_pull: chunk index mismatch - expected " +
                                     std::to_string(chunk_index) +
                                     " got " + std::to_string(hdr.chunk_index));
        }

        uint64_t offset = static_cast<uint64_t>(chunk_index) * CHUNK_SIZE;
        uint64_t chunk_len = std::min(static_cast<uint64_t>(CHUNK_SIZE),
                                      file_meta.file_size - offset);

        // recv chunk data into output file
        t.recv_file(fd.get(), offset, chunk_len);

        // verify chunk hash
        void* mapped = ::mmap(nullptr, chunk_len, PROT_READ, MAP_SHARED,
                              fd.get(), static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::runtime_error("run_pull: mmap for verify failed at chunk " +
                                      std::to_string(chunk_index) +
                                      " - " + std::strerror(errno));
        }

        auto computed = sha256_buf(static_cast<const uint8_t*>(mapped), chunk_len);
        ::munmap(mapped, static_cast<size_t>(chunk_len));

        if (computed == file_meta.chunk_hashes[chunk_index]) {
            cm.mark_done(chunk_index);
        } else {
            cm.mark_todo(chunk_index);
            std::cerr << "\n[pull] hash mismatch at chunk " << chunk_index << "\n";
        }
        PartialFile::save(out, file_meta.chunk_hashes[0], cm);

        std::cout << "[pull] received " << chunk_index + 1
                  << "/" << file_meta.chunk_count << "\r" << std::flush;
    }
    std::cout << "\n";

    if (cm.all_done()) {
        PartialFile::remove(out);
        std::cout << "[pull] transfer complete - saved to " << out << "\n";
    } else {
        uint32_t todo_chunks = static_cast<uint32_t>(cm.needed_chunks().size());
        std::cout << "[pull] transfer incomplete - "
                  << todo_chunks << " chunks failed verify, resume to retry\n";
    }
}