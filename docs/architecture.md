# ElioP2P Architecture Guide

## System Overview

ElioP2P is a distributed P2P cache acceleration system that leverages cluster resources to speed up data loading from object storage.

## Architecture Layers

```
+------------------+
|  Client         |  Application using ElioP2P as proxy
+--------+---------+
         |
+--------v---------+
|  HTTP Proxy     |  Transparent cache layer
+--------+---------+
         |
+--------v---------+
|  Cache Manager  |  L1 Memory + L2 SSD
+--------+---------+
         |
+--------v---------+
|  P2P Network    |  Distributed cache
+--------+---------+
         |
+--------v---------+
|  Control Plane  |  Metadata & coordination
+--------+---------+
         |
+--------v---------+
|  Object Storage|  S3/OSS backend
+------------------+
```

## Components

### 1. HTTP Proxy Server

**Port**: 8080 (default)

Responsible for:
- Accepting HTTP requests from clients
- Parsing URLs to extract cache keys
- Checking local cache
- Forwarding to object storage or P2P network
- Returning responses with cache headers

### 2. Cache Manager

**Layers**:
- L1: Memory cache (LRU, 4-16GB)
- L2: SSD cache (LRU, 100-500GB)

**Features**:
- Multi-factor weighted LRU eviction
- Heat level classification (Hot/Warm/Cold)
- Chunk size: 16MB
- SHA256 integrity verification

### 3. P2P Transfer Module

**Port**: 9000 (default)

**Features**:
- TCP-based communication (no UDP)
- Node discovery via control plane + gossip
- K-selection algorithm (K=5-10)
- Parallel download from multiple peers
- Resumable transfer (checkpoint every 1MB)
- Bandwidth limiting

**Protocol**:
```
[4 bytes: magic = 0x43484B50]
[4 bytes: version]
[4 bytes: message_type]
[4 bytes: chunk_id_length]
[chunk_id_length bytes: chunk_id]
[4 bytes: data_length]
[data_length bytes: data]
[32 bytes: sha256_hash]
[4 bytes: sequence_number]
[1 byte: flags]
```

### 4. Node Discovery

**Modes**:
- ControlPlane: Register with control plane
- GossipOnly: Pure peer-to-peer discovery

**Gossip Messages**:
- NodeJoin: New node announcement
- NodeLeave: Node departure
- ChunkAnnounce: Chunk availability
- ChunkRemove: Chunk removal
- StateSync: Periodic state sync

### 5. Control Plane Client

**API Endpoints**:
- POST /api/v1/nodes - Node registration
- PUT /api/v1/nodes/{id}/heartbeat - Heartbeat
- POST /api/v1/chunks/locations - Query chunk locations
- GET /api/v1/nodes - List nodes
- GET /api/v1/nodes/{id}/commands - Replication commands

### 6. Storage Client

**Supported Backends**:
- S3-compatible (AWS S3, MinIO)
- Alibaba Cloud OSS

**Features**:
- AWS Signature V4
- OSS HMAC-SHA1
- Presigned URL support
- Authentication passthrough

## Data Flow

### Cache Hit
```
Client -> Proxy -> Cache (HIT) -> Client
```

### Cache Miss - Object Storage
```
Client -> Proxy -> Cache (MISS) -> Object Storage -> Cache -> Client
```

### Cache Miss - P2P
```
Client -> Proxy -> Cache (MISS) -> P2P Network -> Cache -> Client
```

## Threading Model

- Main thread: Application lifecycle
- HTTP proxy: Elio coroutine scheduler
- P2P transfer: Elio coroutine scheduler
- Heartbeat: Separate thread
- TCP servers: Separate threads with own schedulers

## Configuration

See [Configuration Guide](configuration.md) for details.
