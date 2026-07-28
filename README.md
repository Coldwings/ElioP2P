# ElioP2P

Distributed P2P cache acceleration for object storage: the first node to
read an object backhauls it from S3; every other node fetches it over the
LAN from that peer. Chunked (16MB) caching with on-demand partial reads,
hedged P2P downloads, and origin-anchored integrity verification.

Built on the [Elio](https://github.com/Coldwings/Elio) C++20 coroutine
framework.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# one node against a local MinIO
ELIOP2P_STORAGE_ENDPOINT=http://localhost:9090 \
ELIOP2P_STORAGE_ACCESS_KEY=minioadmin \
ELIOP2P_STORAGE_SECRET_KEY=minioadmin123 \
./build/bin/ElioP2P --proxy-port 8080 --p2p-port 9000 --p2p-gossip-port 9001

curl http://localhost:8080/test-bucket/some/object -O
```

Watch `X-Cache` and `X-Chunk-Sources` response headers to see which tier
(memory / P2P / origin) served each chunk.

## Documentation

Full docs in [docs/](docs/README.md):

- [Architecture](docs/Architecture.md) — components, data flow, threading, trust model
- [API Contracts](docs/API-Contracts.md) — responsibility boundaries per interface
- [Protocols](docs/Protocols.md) — chunk transfer, gossip, control plane, proxy HTTP
- [API Reference](docs/API-Reference.md) — public signatures
- [Deployment](docs/Deployment.md) — single node, cluster, systemd, Kubernetes
- [Configuration](docs/Configuration.md) — env vars, CLI, defaults
- [Security](docs/Security.md) — trust root, poisoning defense, boundaries
- [Performance Tuning](docs/Performance-Tuning.md)
- [Testing](docs/Testing.md) — unit tests + real-MinIO E2E
- [Development](docs/Development.md) — build, code map, pitfalls

## Requirements

- Linux (io_uring preferred; epoll fallback)
- GCC 12+ or Clang 15+ (C++20), CMake 3.20+
- OpenSSL development files

## License

Proprietary
