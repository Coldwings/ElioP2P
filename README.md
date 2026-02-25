# ElioP2P

Distributed P2P cache acceleration system for large-scale clusters.

## Overview

ElioP2P is a distributed P2P cache acceleration system designed to speed up data loading from object storage in large-scale clusters. It leverages cluster network, memory, and disk resources to provide distributed caching and P2P transfer capabilities.

## Features

- **Multi-tier Caching**: L1 memory cache (LRU), L2 SSD cache, L3 object storage
- **P2P Transfer**: Node discovery, resumable transfer, parallel download, K-selection
- **Storage Support**: S3-compatible (AWS S3, MinIO) and Alibaba Cloud OSS
- **HTTP Proxy**: Transparent caching with authentication passthrough
- **Control Plane**: Node registration, heartbeat, metadata query, replication adjustment

## Architecture

```
+-------------------+
|  HTTP Proxy      |  Port 8080
+--------+----------+
         |
+--------v----------+
|  Cache Manager   |  L1: Memory (4-16GB)
|  Chunk Manager   |  L2: SSD (100-500GB)
+--------+----------+
         |
+--------v----------+
|  P2P Transfer   |  TCP (no UDP)
|  Node Discovery |  Port 9000
+--------+----------+
         |
+--------v----------+
|  Control Plane  |
|  Client         |
+--------+----------+
         |
+--------v----------+
|  Object Storage |
|  (S3/OSS)       |
+-------------------+
```

## Requirements

- C++20 compatible compiler
- CMake 3.20+
- Elio coroutine framework
- OpenSSL (for TLS)
- liburing (optional, for async I/O)

## Build

```bash
mkdir build && cd build
cmake ..
make
```

## Run

```bash
./bin/ElioP2P --help
```

## Configuration

Configuration can be provided via:
- Config file (INI format)
- Environment variables
- Command line arguments

### Environment Variables

| Variable | Description |
|----------|-------------|
| ELIOP2P_NODE_ID | Node identifier |
| ELIOP2P_BIND_ADDRESS | Bind address |
| ELIOP2P_LOG_LEVEL | Log level (DEBUG, INFO, WARNING, ERROR) |
| ELIOP2P_CONTROL_PLANE | Control plane endpoint |
| ELIOP2P_STORAGE_ENDPOINT | Object storage endpoint |
| ELIOP2P_P2P_PORT | P2P listen port |
| ELIOP2P_PROXY_PORT | HTTP proxy port |

## Modules

- **base**: Logger, Config, Error codes
- **cache**: LRU cache, Chunk manager
- **p2p**: Node discovery, Transfer manager
- **control**: Control plane client
- **proxy**: HTTP proxy server
- **storage**: S3/OSS client

## License

Proprietary
