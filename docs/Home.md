# ElioP2P

**ElioP2P** is a distributed P2P cache acceleration system for object storage.
It turns the spare memory, disk, and network bandwidth of a cluster into a
shared read cache in front of S3-compatible storage: the first node to read an
object backhauls it from the origin; every other node fetches it over the LAN
from that peer instead of hitting the origin again.

Built on the [Elio](https://github.com/Coldwings/Elio) C++20 coroutine
framework.

## Features

- **Chunked object caching** — objects are split into fixed-size chunks
  (default 16MB) that cache, distribute, and evict independently
- **On-demand partial reads** — HTTP Range requests map to the covering chunk
  range; only the chunks you ask for are ever fetched
- **Hedged P2P downloads** — a chunk is raced across K peers; the first
  SHA256-verified copy wins and the rest are cancelled
- **Origin-anchored integrity** — per-chunk SHA256 learned from the storage
  origin is propagated with object metadata, so a peer serving wrong bytes
  provably loses the race (cache-poisoning resistant)
- **Zero-copy hot path** — cache hits hand out shared-const chunk handles;
  no data is copied between cache, proxy, and P2P serving
- **Two-tier cache** — in-memory LRU with multi-factor eviction scoring plus
  a disk tier with an in-memory index (lazy promote on hit)
- **Gossip discovery** — chunk announcements and peer state propagate over a
  TCP gossip protocol; manual peer wiring and control-plane discovery
- **S3 / OSS backends** — AWS SigV4 and OSS signing, presigned URL
  generation, path-style and virtual-hosted endpoints
- **S3-proxy HTTP interface** — `GET/HEAD /bucket/key` with Range, 206/416,
  ETag, `X-Cache` and per-chunk `X-Chunk-Sources` provenance headers

## Requirements

- Linux (io_uring preferred; epoll fallback via Elio)
- GCC 12+ or Clang 15+ with C++20
- CMake 3.20+
- OpenSSL development files (SigV4 signing, TLS)

## Quick Start

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run a single node against a local MinIO:

```bash
ELIOP2P_STORAGE_ENDPOINT=http://localhost:9090 \
ELIOP2P_STORAGE_ACCESS_KEY=minioadmin \
ELIOP2P_STORAGE_SECRET_KEY=minioadmin123 \
./build/bin/ElioP2P --proxy-port 8080 --p2p-port 9000 --p2p-gossip-port 9001
```

Fetch through it:

```bash
curl http://localhost:8080/test-bucket/photos/2024/cat.jpg \
     -H 'Range: bytes=0-1048575' -v
# < HTTP/1.1 206 Partial Content
# < X-Chunk-Sources: 0:STORAGE
# second node fetching the same range would answer 0:P2P
```

Run the end-to-end proof suite (two nodes + MinIO + a live poisoning attack):

```bash
./build/bin/e2e_p2p_cluster   # 22 checks: distribution, integrity, attack
./build/bin/e2e_p2p_partial   # 22 checks: on-demand partial reads
```

See [Testing](Testing.md) for how the MinIO fixture is set up.

## Documentation

- [Architecture](Architecture.md) — components, data flow, threading model, trust model
- [API Contracts](API-Contracts.md) — responsibility boundaries for every public interface
- [Protocols](Protocols.md) — chunk transfer protocol, gossip protocol, control-plane API
- [API Reference](API-Reference.md) — public class and method signatures
- [Deployment](Deployment.md) — single node, cluster, systemd, Kubernetes, MinIO lab
- [Configuration](Configuration.md) — environment variables, CLI, and defaults reference
- [Security](Security.md) — trust root, poisoning defense, traversal hardening, allowlist
- [Performance Tuning](Performance-Tuning.md) — chunk size, cache tiers, K value, rate limits
- [Testing](Testing.md) — unit tests, E2E fixtures, debugging probes
- [Development](Development.md) — build layout, code map, contributor notes
