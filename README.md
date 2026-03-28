# embr
A zero-copy large file transfer engine built with C++20, io_uring, and a pluggable transport layer.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — vanilla UDP + io_uring:
- No TLS overhead, io_uring registered buffers, self-managed reliability (seq/ACK/retransmit)
- Target: saturate 10G+ links at <30% single-core CPU

**Public P2P** — ngtcp2 + io_uring:
- TLS 1.3, NAT traversal, token-based peer discovery via tracker
- Same io_uring buffer infrastructure, ngtcp2 QUIC state machine on top

## Usage
```bash
# v0.2 — chunked transfer (current)
embr push <file> [--port PORT]
embr pull <ip>   [--port PORT] [--out PATH]

# v0.4+ — tracker mode
embr push large_dataset.tar.gz   # → token: Kf3xQ9mZ
embr pull Kf3xQ9mZ               # resolves via tracker
embr pull Kf3xQ9mZ 192.168.1.50  # direct mode, skip tracker
```

## How It Works

**Direct mode (v0.2 — current)**
```
Sender                                                     Receiver
  │  embr push file.tar.gz                                     │
  │  → listening on :9000                                      │
  │  ←──────────── HANDSHAKE ───────────────────────────────── │
  │  ──────────── FILE_META ───────────────────────────────→   │
  │  ←──────────── CHUNK_REQ{0} ────────────────────────────── │
  │  ──────── CHUNK_HDR{0,hash} + raw bytes ───────────────→   │
  │  ←──────────── CHUNK_REQ{1} ────────────────────────────── │
  │  ──────── CHUNK_HDR{1,hash} + raw bytes ───────────────→   │
  │  ...                                                        │
  │  ←──────────── COMPLETE ────────────────────────────────── │
```

**Tracker mode (v0.4+)**
```
Sender                        Tracker                      Receiver
  │──── POST /register ───────>│                              │
  │<─── token: Kf3xQ9mZ ──────│                              │
  │                            │<──── GET /resolve/Kf3xQ9mZ ─│
  │                            │───── {addr, size} ──────────>│
  │<══════════ UDP + parallel chunk transfer ════════════════>│
```

## Build
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Dependencies

- C++20 compiler (GCC 12+ / Clang 15+)
- CMake 3.25+
- OpenSSL (SHA256 via EVP interface)
- GoogleTest (fetched via CMake FetchContent)

## Architecture
```
embr/
├── src/
│   ├── main.cpp                   # CLI routing, transport lifecycle
│   ├── core/
│   │   ├── protocol.hpp/.cpp      # send_msg/recv_msg, wire format
│   │   ├── chunk_manager.hpp      # chunk state bitmap
│   │   ├── hash.hpp/.cpp          # SHA256 via OpenSSL EVP
│   │   ├── push.hpp/.cpp          # sharer logic
│   │   └── pull.hpp/.cpp          # fetcher logic
│   ├── transport/
│   │   ├── transport.hpp          # abstract interface
│   │   ├── tcp_transport.hpp/.cpp # TCP implementation
│   │   ├── tcp_client.hpp/.cpp    # tcp_connect() factory
│   │   └── tcp_server.hpp/.cpp    # tcp_listen() / tcp_accept() factories
│   └── util/
│       ├── socket_fd.hpp          # RAII fd wrapper
│       └── constants.hpp          # CHUNK_SIZE, READ_BUF_SIZE
├── tracker/                       # standalone tracker server (v0.4+)
├── tests/
│   └── test_protocol.cpp
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
  CHUNK_REQ  (0x03)  [chunk_index:u32 BE]
  CHUNK_HDR  (0x04)  [chunk_index:u32 BE][chunk_hash:32 bytes] + raw bytes via data plane
  COMPLETE   (0x06)  (no payload)
  ERROR      (0x07)  [reason:utf8]
  CANCEL     (0x08)  (no payload)
```

## Roadmap

| Phase | What |
|-------|------|
| v0.1 | TCP whole-file transfer, pluggable transport, wire protocol ✓ |
| **v0.2** | **16MB chunking + SHA256 per-chunk integrity ✓** |
| v0.3 | TCP + mmap + sendfile(), zero-copy on TCP path |
| v0.4-v0.5 | Vanilla UDP + io_uring, self-managed reliability, 1-to-1 trusted network |
| v0.6-v0.7 | Parallel chunks in flight, sliding window, atomic ChunkManager, Prometheus metrics |
| v0.8+ | ngtcp2 + io_uring, public P2P, TLS 1.3, NAT traversal, 1-to-N fanout |
| v1.x | ngtcp2 + eBPF/XDP, AF_XDP bypass, multi-seeder |

## Current Status

**v0.2 — 16MB chunking + SHA256 per-chunk integrity**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` factories
- [x] `SocketFd` RAII wrapper
- [x] `util/constants.hpp` — `CHUNK_SIZE`, `READ_BUF_SIZE`
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`)
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing via `std::function` release callback
- [x] `ChunkManager` — `vector<bool>` bitmap, bounds-checked, parallel-ready interface
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, `EvpCtx` RAII wrapper
- [x] 16MB chunking — `CHUNK_HDR` + raw bytes via data plane, `pread`/`pwrite` offset-safe
- [x] `ftruncate` pre-allocation — `pwrite` at arbitrary offsets, parallel-ready
- [x] `file_hash` removed — per-chunk SHA256 provides full integrity coverage
- [x] Whole-file + chunked push/pull over TCP
- [x] CLI: `embr push <file>` / `embr pull <ip>`
- [x] Protocol unit tests — `HandshakePayload`, `ChunkReq`, `ChunkHdr` round-trips
- [x] End-to-end validated: 3.2GB ISO to remote VPS, SHA256 matched

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.
```
