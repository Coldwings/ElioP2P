# Architecture

## System Overview

```
                 ┌─────────────────────────────────────────────────┐
                 │                   ElioP2P node                   │
   S3 client ───►│  ┌───────────┐   ┌──────────────────────────┐   │
   GET /bucket/x │  │ Proxy     │──►│ RequestHandler           │   │
   (port 8080)   │  │ (HTTP srv)│   │  handle_request()        │   │
                 │  └───────────┘   └─────┬───────────┬────────┘   │
                 │                        │           │            │
                 │  ┌───────────────┐  ┌──▼────────┐ ┌▼─────────┐  │
                 │  │ ChunkManager  │◄─┤ Transfer- │ │ Storage- │  │
                 │  │  mem LRU +    │  │ Manager   │ │ Client   │  │
                 │  │  disk index   │  │ (P2P)     │ │ (S3/OSS) │  │
                 │  └───────┬───────┘  └──┬─────┬──┘ └────┬─────┘  │
                 │          │             │     │         │        │
                 │  ┌───────▼───────┐  ┌──▼──┐ ┌▼──────┐  │        │
                 │  │ disk files    │  │chunk│ │Node-  │  │        │
                 │  │ chunks/*.chunk│  │srv  │ │Discov.│  │        │
                 │  └───────────────┘  │:9000│ │gossip │  │        │
                 │                     └─────┘ │:9001  │  │        │
                 └─────────────────────────────┼───────┼──┼────────┘
                                               │       │  │
                     other nodes  ◄────────────┘       │  ▼
                     (peers)                      origin storage
```

One process per node. Five ports at most: proxy (8080), chunk transfer
(9000), gossip (9001), control plane client (outbound), control plane server
(8082, only on the designated control-plane node).

## Components

| Component | Responsibility | Owns | Does NOT own |
|-----------|---------------|------|--------------|
| `ProxyServer` | HTTP listener + router + handler coroutines | server thread, `elio::http::server` | cache policy, transfer logic |
| `RequestHandler` | request → cache-key mapping, per-chunk resolution order (cache → P2P → storage), Range assembly, meta chunk lifecycle | resolution policy | storage signing, P2P wire details |
| `ChunkManager` | two-tier cache: memory LRU + disk index/files, chunk metadata, SHA256 | cache data, disk layout | when to fetch what |
| `LRUCache` | keyed store with weighted eviction | eviction scoring | disk persistence |
| `TransferManager` | P2P data plane: hedged download, upload serving, bandwidth limits, cancel | chunk TCP server, peer connections | peer discovery (delegates to NodeDiscovery) |
| `NodeDiscovery` | peer registry, gossip protocol I/O, chunk-availability index | gossip server, peer table | transfer protocol |
| `StorageClient` (S3/OSS) | origin I/O: HEAD/GET(range)/PUT/DELETE/list, SigV4/OSS signing | credentials, HTTP to origin | caching decisions |
| `ControlPlaneServer/Client` | node registry, heartbeat, replication commands with ACK | control-plane state | data-plane traffic |

## Chunk model

An object `<bucket>/<key>` is split at `chunk_size_mb` (16MB default) into
chunks keyed `"<bucket>/<key>_<index>"`. A special meta chunk
`"<bucket>/<key>#meta"` holds `"<size> <etag> [<chunk_sha256_hex> ...]"`.

Every chunk is independently cached, announced, fetched, verified, and
evicted. A Range request maps to `[start/chunk_size .. end/chunk_size]`; only
those chunks are fetched — a partially cached object is normal and expected.
The response aggregates and reports each chunk's tier in the
`X-Chunk-Sources` header (`0:CACHE,1:CACHE,2:STORAGE`).

## Read path

```
GET/HEAD /bucket/key [Range: bytes=a-b]
  1. parse_cache_key → bucket, key  (bucket allowlist enforced here)
  2. get_object_info → size/etag/hash-list
       local "#meta" chunk → P2P download of "#meta" → HEAD to origin
  3. for each covered chunk index:
       local cache hit → CACHE (zero-copy shared handle)
       P2P hedged download → P2P (winner stored + announced)
       origin range GET    → STORAGE (stored, hash anchored in meta, announced)
  4. trim edges, assemble body
     200 / 206 (+ Content-Range) / 416, plus X-Cache and X-Chunk-Sources
```

Step 3 is per-chunk, so one response may mix all three tiers. A failed chunk
fails the whole request with 502; there is no partial-body degradation.

## Write path (P2P upload)

Peers may push chunks via the protocol's `Upload` message. The receiver
verifies the payload SHA256 against the sender-provided header hash before
invoking the registered `chunk_data_consumer` (which stores into
ChunkManager). Content acceptance at the proxy layer is still gated by the
origin-anchored meta hashes on the download path.

## Threading model

- **main thread** — application lifecycle; blocks on a signal flag
- **global Elio scheduler** (N workers) — P2P transfer coroutines, gossip
  loop, download races, blocking-pool for legacy blocking calls
- **proxy server thread** — its own `elio::run` scheduler serving HTTP
- **gossip TCP server thread** — accept loop + handler coroutines on the
  injected scheduler
- **chunk TCP server** — accept loop + per-connection handler coroutines on
  the injected scheduler
- **control-plane heartbeat thread** — periodic registration/heartbeat

Coroutines are lazy: creating a `task` does nothing until `co_await`ed or
`scheduler->spawn(...)`ed. Every long-running loop in the codebase is spawned
on the injected scheduler; nothing self-schedules.

## Trust model

```
origin storage (signed channel, configured credentials)
      │  first backhaul computes SHA256 per chunk
      ▼
meta chunk ("key#meta": size, etag, chunk_hash[])
      │  distributes over gossip like any chunk
      ▼
any node's expected_sha256 check inside download_from_peer
      → a peer serving wrong bytes loses the race even with a
        consistent self-reported transfer hash
```

The origin is the only trust root. Transfer-level hashes detect corruption;
origin-anchored hashes detect malicious content. Until a chunk's hash has
been learned from the origin, that chunk is weak-trust (first backhaul wins);
afterwards it is strong-verified everywhere.

## Failure semantics

| Failure | Behavior |
|---------|----------|
| peer connect timeout mid-race | that peer loses; others continue |
| winning peer's data corrupt/mismatched | peer loses race, next winner tried |
| all P2P candidates fail | transparent storage backhaul |
| storage unreachable AND P2P fails | 502/503 to the client |
| node crash with pending replication commands | server redelivers after 60s ACK timeout |
| gossip announce lost | periodic StateSync re-converges (gossip interval) |
