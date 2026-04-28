# embr
A zero-copy large file transfer engine built with C++20, zero-copy I/O (sendfile + splice / io_uring), and a pluggable transport layer.

*From Old English ǣmyrġe, "smoldering ash." A shared file is like an ember: still glowing, passed from hand to hand, never fully extinguished.*

## Why

Existing tools arrange disk I/O and network I/O sequentially — the disk waits for the network, the network waits for the disk. Add redundant memory copies between kernel and userspace, and even 10G links stay half-idle.

embr pipelines disk and network operations in parallel and eliminates intermediate copies, targeting near-line-rate throughput at minimal CPU overhead.

## Two Transfer Modes

**Trusted network (LAN / datacenter)** — TCP with kernel-bypass I/O:
- `sendfile()` on push (0 copies), `splice()` on pull (0 copies)
- SO_SNDBUF/SO_RCVBUF sized to bandwidth-delay product, TCP_NODELAY on control messages
- io_uring UDP data plane available (benchmarked, closed pending single-transport refactor)

**Public P2P** — ngtcp2 + io_uring (planned):
- TLS 1.3, NAT traversal, token-based peer discovery via tracker
- Same io_uring buffer infrastructure, ngtcp2 QUIC state machine on top

## Usage

```bash
# Direct mode — push listens, pull connects by IP
embr push <file> [--port PORT]
embr pull <ip>   [--port PORT] [--out PATH]

# Tracker mode — push registers a token, pull resolves it
embr push <file> --tracker http://104.194.95.77:10009
embr pull <token> --tracker http://104.194.95.77:10009

# Trust a tracker — persists URL, no --tracker flag needed after
embr trust http://104.194.95.77:10009
embr push <file>     # auto-registers token
embr pull <token>    # auto-resolves via trusted tracker

# Run your own tracker
embr tracker [--bind ADDR] [--port PORT] [--ttl MINUTES]

# Manage trusted tracker
embr trust --show    # print current trusted tracker
embr trust --clear   # remove saved tracker
```

## How It Works

**v0.6 — token + tracker**
```
Sender                        Tracker (plurb.org:10009)     Receiver
  │  embr push file.bin                                         │
  │  → precompute SHA256 (cached in .embr.hash)                 │
  │  → tcp_listen(:10007)                                       │
  │──── POST /register ──────>│                                 │
  │<─── 200 {sender_ip_recorded} ───────────────────────────────│
  │  [push] token: 67c49d1e7b5c9a6a                             │
  │  → waiting for connection                                   │
  │                            │<──── GET /resolve/:token ───── │
  │                            │───── {sender_ip, port} ──────> │
  │<══════ TCP transfer (request-driven, resume-capable) ══════>│
  │──── POST /unregister ─────>│                                │
```

**v0.5 — request-driven transfer with resume**
```
Sender                                                        Receiver
  │  embr push file.tar.gz                                        │
  │  → precompute SHA256 for all chunks                           │
  │  → listening on :10007                                        │
  │  ←──────────── HANDSHAKE ──────────────────── [TCP] ────────  │
  │  ──── FILE_META {filename, size, chunk_hashes[0..N]} ───────→ │
  │                                      check .embr.partial      │
  │  ←──────────── CHUNK_REQ{i0} ─────────────── [TCP] ────────   │
  │  ←──────────── CHUNK_REQ{i1} ─────────────── [TCP] ────────   │
  │  ←──────────── COMPLETE ───────────────────── [TCP] ────────  │
  │  ──── CHUNK_HDR{i0} + sendfile(chunk i0) ──────────────────→  │
  │                                      verify SHA256 → save     │
  │  ──── CHUNK_HDR{i1} + sendfile(chunk i1) ──────────────────→  │
  │                                      verify SHA256 → save     │
  │  ...                                                          │
```

Push answers requests and holds no session state — stateless across connections. Resume state lives entirely on the pull side in `.{filename}.embr.partial`. Token is derived from file content (`SHA256(concat(chunk_hashes))`), first 8 bytes hex-encoded — same file always produces same token.

## Benchmark

Localhost, 2.6GB Fedora ISO:
Localhost, 2.6GB Fedora ISO, `MTU=65000`:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr TCP + sendfile/splice | 2.21s | 1.13s | 0.91s | 1.27 GB/s |
| embr UDP + io_uring | 2.113s | 1.142s | 0.775s | 1.22 GB/s |
| scp | 1.20s | 0.31s | 1.19s | 2.22 GB/s |

embr has 24% lower sys time than scp on localhost. scp's lower wall time reflects no integrity verification — embr verifies every chunk with SHA256. scp performs no data integrity checking.

UDP sys time matches TCP (0.775s vs 0.709s on the 3GB run) — io_uring kernel overhead eliminated at large MTU. Remaining wall-time gap vs TCP = stop-and-wait ACK latency (2616 chunks × TCP RTT). Sliding window deferred to ngtcp2 phase where QUIC streams provide it for free. UDP path closed in v0.6 pending single-transport interface refactor — benchmarked and validated, not exposed in CLI.

Remote VPS, 100MB file, ~5 MB/s uplink:

| Tool                       | Avg Wall | Throughput |
|----------------------------|----------|------------|
| embr TCP + sendfile/splice | 19.9s | 5.0 MB/s |
| scp                        | 22.5s | 4.4 MB/s |

embr is **12% faster than scp** on a real WAN link with full SHA256 integrity verification. scp variance is high (16s–27s) due to TLS + compression fluctuation; embr variance is tight (18.5s–20.6s).

Remote VPS, 3GB file, ~25 MB/s uplink:

| Tool | Wall | User | Sys | Throughput |
|------|------|------|-----|------------|
| embr TCP + sendfile | 131.2s | 2.0s | 8.9s | 23.4 MB/s |
| scp | 117.7s | 3.6s | 12.5s | 26.1 MB/s |

embr TCP uses 44% less userspace CPU than scp on the remote path — SHA256 pre-computed before transfer begins, not in the hot path.

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

**memlock limit:** io_uring registered buffers require sufficient locked memory. Default 8MB is sufficient for `CHUNK_SIZE=1MB`. Verify with `ulimit -l`.

## Architecture
```
embr/
├── src/
│   ├── main.cpp                        # verb dispatch (~20 lines)
│   ├── cli/
│   │   ├── push_cli.hpp/.cpp           # push argparse, token derivation, tracker register
│   │   ├── pull_cli.hpp/.cpp           # pull argparse, token detection, tracker resolve
│   │   ├── tracker_cli.hpp/.cpp        # tracker argparse, run_tracker_server
│   │   └── trust_cli.hpp/.cpp          # trust argparse, ~/.config/embr/tracker
│   ├── core/
│   │   ├── protocol.hpp/.cpp           # send_msg/recv_msg, wire format
│   │   ├── chunk_manager.hpp/.cpp      # runtime bitmap of completed chunks
│   │   ├── partial_file.hpp/.cpp       # .embr.partial serialize/deserialize
│   │   ├── hash.hpp/.cpp               # SHA256 + .embr.hash cache
│   │   ├── push.hpp/.cpp               # sender logic
│   │   └── pull.hpp/.cpp               # receiver logic, resume
│   ├── tracker/
│   │   ├── token_store.hpp/.cpp        # in-memory token→(ip,port) map + TTL
│   │   ├── tracker_handlers.hpp/.cpp   # HTTP handler logic
│   │   ├── tracker_server.hpp/.cpp     # cpp-httplib wiring, SIGINT shutdown
│   │   └── tracker_client.hpp/.cpp     # register/resolve/unregister HTTP client
│   ├── transport/
│   │   ├── transport.hpp               # abstract interface
│   │   ├── tcp_transport.hpp/.cpp      # sendfile() push, splice() pull
│   │   ├── tcp_client.hpp/.cpp         # tcp_connect() factory
│   │   ├── tcp_server.hpp/.cpp         # tcp_listen() / tcp_accept() factories
│   │   ├── udp_transport.hpp/.cpp      # UDP + io_uring implementation
│   │   ├── udp_data_client.hpp/.cpp    # udp_data_client_connect factory
│   │   └── udp_data_server.hpp/.cpp    # udp_data_server_bind/connect factories
│   └── util/
│       ├── socket_fd.hpp               # RAII fd wrapper
│       ├── exact_io.hpp                # send_exact/recv_exact, fd_read/write_exact
│       ├── json_parser.hpp             # hand-rolled flat JSON helpers
│       ├── config_tracker.hpp          # resolve_tracker_url, read/write config
│       ├── io_uring_ctx.hpp/.cpp       # io_uring ring + registered buffer pool
│       └── constants.hpp               # CHUNK_SIZE, HASH_SIZE, EMBR_PORT, TRACKER_PORT
├── tests/
│   ├── test_protocol.cpp
│   ├── test_tcp.cpp
│   ├── test_udp.cpp
│   ├── test_resume.cpp
│   └── test_tracker.cpp
└── CMakeLists.txt
```

Business logic (`push`, `pull`) talks only to `Transport&` — never to raw sockets. Transport lifecycle owned by CLI layer. Swapping transport = one file change.

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

## Resume

Interrupted transfers continue from the last verified chunk. Progress is persisted in `.{filename}.embr.partial` alongside the output file:
```
[file_hash: 32 bytes][bitmap: ceil(chunk_count/8) bytes, LSB = chunk 0]
```

On restart, pull loads the bitmap, requests only missing chunks, verifies SHA256 per chunk before marking done. Partial file removed only when `all_done()`. Delete `.{filename}.embr.partial` manually to force a fresh transfer.

## Tracker

The tracker is a lightweight HTTP server mapping content-derived tokens to sender addresses. Token = `SHA256(concat(chunk_hashes))`, first 8 bytes hex-encoded — deterministic from file content, same file always produces same token.

```
POST /register          {token, sender_port} → 200 {sender_ip_recorded}
GET  /resolve/:token    → 200 {sender_ip, sender_port} or 404
POST /unregister/:token → 204
```

Tracker stores no file data — pure `token → (ip, port)` indirection. File integrity guaranteed end-to-end by SHA256 chunk hashes in `FILE_META`. Tokens expire after 10 minutes (configurable with `--ttl`).

## Roadmap

| Phase | What |
|-------|------|
| v0.1 | TCP whole-file transfer, pluggable transport, wire protocol ✓ |
| v0.2 | 16MB chunking + SHA256 per-chunk integrity ✓ |
| v0.3 | TCP + sendfile() + mmap(MAP_SHARED), zero-copy push ✓ |
| v0.4 | UDP + io_uring, registered buffers, direct-to-disk recv ✓ |
| v0.5 | Request-driven protocol, resume interrupted transfers ✓ |
| **v0.6** | **Token + tracker, splice() zero-copy recv, .embr.hash cache, embr trust ✓** |
| v0.7 | Persistent push daemon, multi-sender tokens, HTTPS tracker |
| v0.8+ | ngtcp2 + io_uring, TLS 1.3, NAT traversal, sliding window, 1-to-N fanout |
| v1.x | ngtcp2 + eBPF/XDP, AF_XDP bypass, multi-tracker federation |

## Current Status

**v0.6 — token + tracker**

- [x] Project skeleton, CMake, GoogleTest
- [x] Pluggable `Transport` interface — control plane `send`/`recv`, data plane `send_file`/`recv_file`
- [x] `TcpTransport` + `tcp_connect` / `tcp_listen` / `tcp_accept` factories
- [x] `TcpTransport::send_file` — `sendfile()` syscall, 0 copies push
- [x] `TcpTransport::recv_file` — `splice()` socket→pipe→file, 0 copies pull
- [x] `UdpTransport` — io_uring registered buffers, READ_FIXED + sendmsg, RECV + WRITE_FIXED direct-to-disk
- [x] TCP socket tuning — `SO_SNDBUF`/`SO_RCVBUF`=4MB, `TCP_NODELAY`
- [x] `SocketFd` RAII wrapper
- [x] `util/io.hpp` — `send_exact`/`recv_exact`, `fd_read_exact`/`fd_write_exact`
- [x] `util/constants.hpp` — `CHUNK_SIZE`, `HASH_SIZE`, `EMBR_PORT=10007`, `TRACKER_PORT=10009`
- [x] Custom binary wire protocol (`protocol.hpp/.cpp`), `PROTOCOL_VERSION=0x02`
- [x] `Buffer` — move-only, unified heap/mmap/io_uring backing via `std::function` release callback
- [x] `hash.hpp/.cpp` — SHA256 via OpenSSL EVP, `.embr.hash` cache (invalidates on mtime+size change)
- [x] Pre-committed chunk hashes — all SHA256s computed before transfer, embedded in `FILE_META`
- [x] `ChunkManager` — `vector<bool>` bitmap, bounds-checked, `mark_done`/`mark_todo`/`needed_chunks`/`all_done`
- [x] `PartialFile` — `.embr.partial` serialize/deserialize, hash + output file validation on load
- [x] Request-driven protocol — pull sends `CHUNK_REQ` per needed chunk + `COMPLETE`; push answers, stateless
- [x] Resume — per-chunk verify + save; partial removed only on `all_done()`; fresh/resume share one code path
- [x] `ftruncate` pre-allocation — recv at arbitrary offsets, parallel-ready
- [x] Token derivation — `SHA256(concat(chunk_hashes))`, first 8 bytes hex, content-derived, deterministic
- [x] `TokenStore` — `unordered_map<token, Record>` + mutex + TTL eviction + background sweeper
- [x] Tracker HTTP server — `POST /register`, `GET /resolve/:token`, `POST /unregister/:token`
- [x] Tracker client — `tracker_register`/`tracker_resolve`/`tracker_unregister` (noexcept unregister)
- [x] `embr trust` — persist tracker URL to `~/.config/embr/tracker`, auto-used by push/pull
- [x] Subcommand dispatch — `embr push` / `embr pull` / `embr tracker` / `embr trust`
- [x] CLI: full argparse per verb, `EMBR_TRACKER` env var, `--tracker` flag override
- [x] Protocol unit tests, transport unit tests, resume unit tests, tracker unit tests
- [x] End-to-end demo: public internet transfer via token (RackNerd VPS → Fedora laptop)

## License

[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/) — Modify embr's files → your changes must be open source. Use embr in your own project → your new files can be any license.