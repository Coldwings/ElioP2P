# API Contracts

This page defines ElioP2P's public-interface responsibility boundaries, in
the same style as Elio's own API Contracts. Use it when triaging issues,
reviewing security-sensitive behavior, or integrating ElioP2P components
into another application.

- "**ElioP2P guarantees**" describes library-side behavior. A regression
  against it is a bug.
- "**Caller must guarantee**" describes application preconditions. Violating
  them is caller misuse, not a library bug.
- If no row says an object is safe for concurrent use, callers must
  serialize access to it.
- Protocol bytes from peers are a library boundary; the meaning a client
  assigns to an object key or bucket is an application boundary.

## Cross-cutting defaults

| Area | ElioP2P guarantees | Caller must guarantee |
|------|--------------------|-----------------------|
| Coroutine tasks | Public coroutine interfaces are lazy: they make progress only when awaited or spawned on a running `elio::runtime::scheduler`. Spawning transfers frame ownership to the runtime. | Keep referenced objects (cache, storage, discovery, callbacks) alive until the task completes. Do not drop a lazy task you expect to run — it is destroyed unstarted. |
| Schedulers | Components use the scheduler injected via their `set_scheduler()` for spawning work; nothing silently creates hidden schedulers when one is provided. | Call `set_scheduler()` before `start()` on TransferManager/NodeDiscovery/ProxyServer, and keep the scheduler alive until after the component's `stop()` returns. |
| Thread safety | Types documented here as thread-safe protect their own internal state. | Setter methods (`set_transfer_manager`, `set_p2p_fallback_enabled`, `set_allowed_bucket`) are safe against concurrent request coroutines; all other mutating configuration must happen before `start()`. |
| Errors | Operations report failure through the documented return type (`optional`, `bool`, HTTP status). Coroutine bodies do not leak exceptions across API boundaries. | Check results. An `optional` miss and a transport failure are distinct outcomes only where documented. |
| Data ownership | Cache hits hand out `shared_ptr<const ...>` handles; the cache never hands out mutable aliases of stored data. | Treat all received chunk data as immutable. Do not mutate buffers passed to `store_chunk` until the call returns (it copies). |

## Cache

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `ChunkManager::get_chunk` | Checks memory then disk-index; on a disk hit the data is loaded and promoted to memory. Returns `shared_ptr<const Chunk>` — zero-copy, safe for concurrent readers, and valid as long as the caller holds it even if the entry is later evicted. All internal state guarded by `shared_mutex`. | Chunk ids passed in are treated as opaque keys. Do not rely on call ordering between concurrent `get_chunk`/`store_chunk` for the same id beyond last-writer-wins. |
| `ChunkManager::store_chunk` | Copies the data into the cache; when a disk path is configured, persists to disk atomically-ish (direct write) and indexes it. Rejects entries larger than memory capacity (returns false) but still persists them to disk when configured. | Metadata size field, when non-zero, must match the data size or the store is rejected. Data buffer must stay valid for the call duration. |
| `ChunkManager::verify_chunk` | Computes SHA256 of the supplied data and compares with stored metadata hash; returns true when no metadata exists (cannot verify). | Do not treat "no metadata" as a positive integrity result. |
| `LRUCache::get/put/remove` | Fully thread-safe (internal recursive mutex). `put` refuses items larger than total capacity instead of overflowing. Eviction samples the 32 coldest entries and scores by time/replica/heat weights. | Tune weights and thresholds for your workload; eviction is approximate-LRU by design, not exact. |
| `ChunkManager::sync_metadata` | Writes all chunk metadata as JSON via temp-file + rename. Returns false when no disk path is configured. | Call it at shutdown or periodically if metadata durability across restarts matters. |

## P2P transfer

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `TransferManager::start/stop` | `start()` spawns the chunk TCP server on the injected scheduler and marks the manager running. `stop()` signals the accept loop, wakes it with a self-connect, and waits (bounded) for exit. Idempotent. | Set scheduler, node discovery, and both chunk-data callbacks **before** `start()`. Do not call `download_chunk` after `stop()`. |
| `TransferManager::download_chunk` | Races the request across K peers on private buffers. The first peer returning a complete, transfer-hash-verified and (when `expected_sha256` is set) origin-hash-verified copy wins; losers observe `stop_flag` and abort. Falls back through remaining candidates. Returns the winning bytes or nullopt. | `request.expected_sha256`, when known, must be the origin-truth hash (e.g. from the meta chunk); a wrong value can make every honest peer lose. Keep `this` alive through the await. |
| `TransferManager::upload_chunk` | Sends an `Upload` message carrying sender-computed SHA256; succeeds only on a hash-echoing `Ack` from the peer. | The target peer accepts uploads only if it registered a `chunk_data_consumer`. |
| `TransferManager::cancel_transfer` | Sets the shared stop flag for all in-flight peer downloads of that chunk and checkpoints progress for resume. It is a real cancellation: download loops poll the flag between 256KB slices. | Cancellation is cooperative; a peer stalled on connect may take until connect timeout to observe it. |
| `TransferManager::set_chunk_data_provider / consumer` | The chunk TCP server serves peers only through these callbacks. Without a provider, every peer request is an Error response; without a consumer, uploads are rejected. | Callbacks must be thread-safe (they run on scheduler workers) and non-blocking (they are on the data path). |
| `BandwidthLimiter` | Config value `0` means unlimited (no sleeping). Positive values throttle to approximately N Mbps with a 1-second refill window. | Do not share one limiter across directions if independent accounting matters; upload and download have separate limiters already. |

## Node discovery / gossip

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `NodeDiscovery::start/stop` | `start()` marks the service up. Gossip protocol and the gossip TCP listener are spawned by `register_node()`, not by `start()`. `stop()` stops gossip, wakes the accept loop (self-connect), and joins it. | Call `set_scheduler()` before `start()`, and `register_node()` once your local ports are known — the gossip listener only comes up there. |
| `NodeDiscovery::announce_chunk` | Records the chunk locally and broadcasts a `ChunkAnnounce` to a bounded random subset of peers immediately (not at the next tick). Delivery is at-least-once: periodic StateSync re-converges lost announcements. Duplicate deliveries are deduped by content-hash message id. | Only announce chunks actually retrievable from this node; peers will race-download them from you. |
| `NodeDiscovery::get_peers_with_chunk` | Returns the current local view of peers believed to hold the chunk. The view is eventually consistent, not authoritative. | Handle an empty list (fall back to origin) and stale hits (a peer may have lost the chunk or gone offline). |
| `NodeDiscovery::add_peer / remove_peer` | Manual peer wiring; the peer becomes gossip and download eligible immediately. | Intended for bootstrap and tests. In production prefer control-plane discovery; a manually added peer is trusted like any gossip peer. |
| `NodeDiscovery::register_node` | Stores local identity, spawns the gossip TCP listener (on `config.gossip_port`), starts the periodic gossip loop (state sync + heartbeat ticks), and broadcasts `NodeJoin`. | The `PeerNode` passed must carry correct externally reachable `port` (chunk) and `gossip_port`; wrong values poison every peer's view of you. |

## Proxy / request handling

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `RequestHandler::handle_request` | GET and HEAD only; other methods get 405. GET resolves `size/etag` then fetches every covered chunk independently (cache → P2P → storage). Range forms `bytes=a-b`, `bytes=a-`, `bytes=-b` are supported (first range only); invalid ranges get 416 with `Content-Range: bytes */size`. Any chunk that fails all three tiers fails the request with 502/404. Response carries `X-Cache`, per-chunk `X-Chunk-Sources`, `Accept-Ranges`, `ETag`, and (for 206) `Content-Range`. | The request path must be `/bucket/key`; deeper key segments are fine. Do not send bodies or query-dependent behavior — query strings are ignored for caching by design. |
| `RequestHandler::set_allowed_bucket` | When non-empty, requests for any other bucket get 403 before any backend traffic. | Set at startup. When unset, the proxy serves any bucket reachable through the storage credentials — deploy accordingly. |
| `ProxyServer::start/stop` | `start()` binds the HTTP server on a dedicated thread and returns after a readiness delay. `stop()` flips the running flag and joins the thread (bounded by the server's poll loop). | Call `set_transfer_manager()` before traffic if P2P serving is required; without it, requests degrade to cache+storage only. |

## Storage

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `S3Client::get_object(bucket, key, offset, length)` | Performs a signed (SigV4) range GET when credentials are configured; unsigned when not (public buckets / anonymous MinIO). `length == 0` means whole object. Returns nullopt on any non-200/206. | Credentials must be valid for the bucket. `offset+length` beyond object size is origin-defined behavior. |
| `S3Client::head_object` | Signed HEAD; maps 404 to nullopt, other statuses to nullopt with an error log. | ETag presence/format is origin-specific; do not assume MD5 semantics (multipart etags contain a dash). |
| `S3Client::generate_presigned_url` | Produces a spec-conformant SigV4 presigned URL (`SignedHeaders=host`, `UNSIGNED-PAYLOAD`). | The URL inherits this client's credentials and region; expiry starts now. |
| `StorageClientFactory::create` | Picks S3 or OSS by `config.type`; returns nullptr on unknown type. Endpoint scheme (`http://`/`https://`) overrides `use_https`. | Configure `type`, `endpoint`, and (for private buckets) both keys. |

## Control plane

| Interface | ElioP2P guarantees | Caller must guarantee |
|-----------|--------------------|-----------------------|
| `ControlPlaneServer` | Node registry with heartbeat liveness; chunk index updates are deadlock-free (locked variants split); replication commands survive delivery until ACKed and are redelivered after 60s without ACK. Handlers hold a shared ownership of server state, so in-flight requests cannot dangle after destruction. | Run at most one control-plane node in this version; registry persistence uses the metadata manager's JSON file when configured. |
| `ControlPlaneClient::fetch_replication_commands` | Delivers commands to the subscribed callback first and ACKs only after it returns; a throwing callback skips the ACK (server redelivers — at-least-once). Without a callback, commands are ACKed after parsing. | Make the callback idempotent — a crash between processing and ACK means the command will be delivered again. |
| `ControlPlaneClient::ack_command` | Explicit ACK for asynchronous processing flows. | Only ACK after the command's effects are durable. |
