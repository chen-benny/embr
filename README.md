# embr
A zero-copy large file transfer engine built with C++20, zero-copy I/O (send_file / io_uring), and a pluggable transport layer.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — TCP with kernel-bypass I/O:
- `sendfile()` on push (0 copies), `mmap(MAP_SHARED)` on pull (1 copy)
- SO_SNDBUF/SO_RCVBUF sized to bandwidth-delay product, TCP_NODELAY on control messages
- io_uring UDP data plane available (benchmarked, closed pending single-transport refactor)

**Public P2P** — ngtcp2 + io_uring (planned):
- TLS 1.3, NAT traversal, token-based peer discovery via tracker
- Same io_uring buffer infrastructure, ngtcp2 QUIC state machine on top

## Usage
```bash
# Push a file — listens for incoming connection
embr push <file> [--port PORT]

# Pull a file — resumes from last verified chunk if interrupted
embr pull <ip> [--port PORT] [--out PATH]

# v0.6+ — tracker mode (planned)
embr push large_dataset.tar.gz --tracker https://tracker.embr.dev  # → token: Kf3xQ9mZ
embr pull Kf3xQ9mZ --tracker https://tracker.embr.dev
```

## How It Works

**v0.5 — request-driven transfer with resume**
```
Sender                                                        Receiver
  │  embr push file.tar.gz                                        │
  │  → precompute SHA256 for all chunks                           │
  │  → listening on :9000                                         │
  │  ←──────────── HANDSHAKE ──────────────────── [TCP] ────────  │
  │  ──── FILE_META {filename, size, chunk_hashes[0..N]} ───────→ │
  │                                      check .embr.partial      │
  │  ←──────────── CHUNK_REQ{i0} ─────────────── [TCP] ────────  │
  │  ←──────────── CHUNK_REQ{i1} ─────────────── [TCP] ────────  │
  │  ←──────────── ... ────────────────────────── [TCP] ────────  │
  │  ←──────────── COMPLETE ───────────────────── [TCP] ────────  │
  │  ──── CHUNK_HDR{i0} + sendfile(chunk i0) ──────────────────→ │
  │                                      verify SHA256 → save     │
  │  ──── CHUNK_HDR{i1} + sendfile(chunk i1) ──────────────────→ │
  │                                      verify SHA256 → save     │
  │  ...                                                          │
```

Pull sends one `CHUNK_REQ` per needed chunk (all N on fresh transfer, subset on resume), then `COMPLETE` as end-of-requests terminator. Push answers requests and holds no session state — stateless across connections. Resume state lives entirely on the pull side in `.{filename}.embr.partial`.

**Tracker mode (v0.6+ — planned)**
```
Sender                        Tracker                       Receiver
  │──── POST /register ───────>│                               │
  │<─── token: Kf3xQ9mZ ───────│                               │
  │                            │<──── GET /resolve/Kf3xQ9mZ ── │
  │                            │───── {addr, port} ───────────>│
  │<══════════ transfer ═══════════════════════════════════════>│
```

## Benchmark

Localhost, 3GB file (Fedora ISO), `MTU=65000`:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr UDP + io_uring | 2.113s | 1.142s | 0.775s | 1.22 GB/s |
| embr TCP + sendfile | 1.851s | 1.092s | 0.709s | 1.39 GB/s |
| scp | 1.064s | 0.295s | 0.936s | 2.28 GB/s |

Remote VPS, 3GB file, ~25 MB/s uplink:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr TCP + sendfile | 131.2s | 2.0s | 8.9s | 23.4 MB/s |
| scp | 117.7s | 3.6s | 12.5s | 26.1 MB/s |

embr TCP uses 44% less userspace CPU than scp on the remote path (2.0s vs 3.6s user) — SHA256 is pre-computed before transfer begins, not in the hot path.

UDP sys time matches TCP (0.775s vs 0.709s) — io_uring kernel overhead eliminated at large MTU. Remaining wall-time gap vs TCP = stop-and-wait ACK latency (2616 chunks × TCP RTT). Sliding window deferred to ngtcp2 phase where QUIC streams provide it for free. Gap vs scp = SHA256 chunk verification cost. scp performs no integrity verification.

## Build
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Install Dependencies

**Fedora / RHEL / Rocky:**
```bash
sudo dnf install cmake ninja-build gcc-c++ openssl-devel liburing-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install cmake ninja-build g++ libssl-dev liburing-dev
```

**macOS (development only — io_uring not supported, TCP path only):**
```bash
brew install cmake ninja openssl
```

**Kernel requirement:** Linux 6.0+ for io_uring RECV/WRITE_FIXED on UDP sockets. Older kernels fall back to unregistered buffers automatically.

**memlock limit:** io_uring registered buffers require sufficient locked memory. Default 8MB is sufficient for `CHUNK_SIZE=1MB` (2MB chunk pool + 2MB frag pool). Verify with `ulimit -l`.

## Architecture
```
embr/
├── src/
│   ├── main.cpp                        # CLI routing, transport lifecycle
│   ├── core/
│   │   ├── protocol.hpp/.cpp           # send_msg/recv_msg, wire format
│   │   ├── chunk_manager.hpp/.cpp      # runtime bitmap of completed chunks
│   │   ├── partial_file.hpp/.cpp       # .embr.partial serialize/deserialize
│   │   ├── hash.hpp/.cpp               # SHA256 via OpenSSL EVP
│   │   ├── push.hpp/.cpp               # sharer logic
│   │   └── pull.hpp/.cpp               # fetcher logic, resume
│   ├── transport/
│   │   ├── transport.hpp               # abstract interface
│   │   ├── tcp_transport.hpp/.cpp      # TCP: sendfile() push, mmap pull
│   │   ├── tcp_client.hpp/.cpp         # tcp_connect() factory
│   │   ├── tcp_server.hpp/.cpp         # tcp_listen() / tcp_accept() factories
│   │   ├── udp_transport.hpp/.cpp      # UDP + io_uring implementation
│   │   ├── udp_bind.hpp/.cpp           # udp_data_server_bind/connect factories
│   │   └── udp_connect.hpp/.cpp        # udp_data_client_connect factory
│   └── util/
│       ├── socket_fd.hpp               # RAII fd wrapper
│       ├── io.hpp                      # send_exact / recv_exact
│       ├── io_uring_ctx.hpp/.cpp       # io_uring ring + registered buffer pool
│       └── constants.hpp              # CHUNK_SIZE, HASH_SIZE, UDP_MTU
├── tests/
│   ├── test_protocol.cpp
│   ├── test_tcp.cpp
│   ├── test_udp.cpp
│   └── test_resume.cpp
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets.
Transport lifecycle owned by `main.cpp`. Swapping transport = one file change.

## Wire Protocol
```
Header (6 bytes, always): [version:u8][type:u8][payload_len:u32 BE]

Message types:
  HANDSHAKE  (0x01)  [token_len:u32 BE][token:utf8]
  FILE_META  (0x02)  [file_size:u64 BE][filename_len:u32 BE][filename:utf8]
                     [chunk_size:u32 BE][chunk_count:u32 BE]
                     [chunk_hash[0]:32B]...[chunk_hash[N-1]:32B]
  CHUNK_REQ  (0x03)  [chunk_index:u32 BE]  — pull → push, one per needed chunk
  CHUNK_HDR  (0x04)  [chunk_index:u32 BE]  — push → pull, precedes raw chunk data
  COMPLETE   (0x06)  (no payload)          — pull → push, end-of-requests terminator
  ERROR      (0x07)  [reason:utf8]
  CANCEL     (0x08)  (no payload)
```

UDP fragment format (10 bytes, prepended to each datagram):
```
[chunk_index:u32 BE][frag_index:u32 BE][frag_len:u16 BE]
```

## Resume

Interrupted transfers continue from the last verified chunk. Progress is persisted in `.{filename}.embr.partial` alongside the output file:
```
[file_hash: 32 bytes][bitmap: ceil(chunk_count/8) bytes, LSB = chunk 0]
```

On restart, pull loads the bitmap, requests only the missing chunks, and verifies each chunk's SHA256 before marking it done. The partial file is removed only when all chunks pass verification. Delete `.{filename}.embr.partial` manually to force a fresh transfer.

## Roadmap

| Phase | What |
|-------|------|
| v0.1 | TCP whole-file transfer, pluggable transport, wire protocol ✓ |
| v0.2 | 16MB chunking + SHA256 per-chunk integrity ✓ |
| v0.3 | TCP + sendfile() + mmap(MAP_SHARED), zero-copy data plane ✓ |
| v0.4 | UDP + io_uring, registered buffers, direct-to-disk recv ✓ |
| **v0.5** | **Request-driven protocol, resume interrupted transfers ✓** |
| v0.6 | Token + tracker HTTP server, embr push → token, embr pull \<token\> |
| v0.7+ | ngtcp2 + io_uring, public P2P, TLS 1.3, sliding window, 1-to-N fanout |
| v1.x | ngtcp2 + eBPF/XDP, AF_XDP bypass, multi-seeder |

## Current Status

**v0.5 — request-driven resume**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` factories
- [x] `TcpTransport::send_file` — `sendfile()` syscall, 0 copies push
- [x] `TcpTransport::recv_file` — `mmap(MAP_SHARED)` + `recv`, 1 copy pull
- [x] `UdpTransport` — io_uring registered buffers, READ_FIXED + sendmsg, RECV + WRITE_FIXED direct-to-disk
- [x] `SocketFd` RAII wrapper
- [x] `util/io.hpp` — `send_exact` / `recv_exact`
- [x] `util/constants.hpp` — `CHUNK_SIZE=1MB`, `HASH_SIZE`, `UDP_MTU`
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`), `PROTOCOL_VERSION=0x02`
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing via `std::function` release callback
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, `EvpCtx` RAII wrapper
- [x] Pre-committed chunk hashes — all SHA256s computed before transfer, embedded in `FILE_META`
- [x] `ChunkManager` — `vector<bool>` bitmap, bounds-checked, `mark_done`/`mark_todo`/`needed_chunks`/`all_done`
- [x] `PartialFile` — `.embr.partial` serialize/deserialize, hash + output file validation on load
- [x] Request-driven protocol — pull sends `CHUNK_REQ` per needed chunk + `COMPLETE`; push answers, stateless
- [x] Resume — per-chunk verify + save; partial removed only on `all_done()`; fresh/resume share one code path
- [x] TCP socket tuning — `SO_SNDBUF`/`SO_RCVBUF`=4MB, `TCP_NODELAY`
- [x] `ftruncate` pre-allocation — `pwrite` at arbitrary offsets, parallel-ready
- [x] CLI: `embr push <file> [--port PORT]` / `embr pull <ip> [--port PORT] [--out PATH]`
- [x] Protocol unit tests — round-trips, malicious chunk_count guard, Buffer ownership
- [x] Transport unit tests — echo, aligned/unaligned send_file/recv_file, UDP multi-chunk
- [x] Resume unit tests — ChunkManager invariants, PartialFile round-trip, hash mismatch, missing output file

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.