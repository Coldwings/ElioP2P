# Performance Tuning

Measured on loopback (E2E fixtures): origin backhaul ≈ 300ms for 20MB,
P2P fetch ≈ 130ms, local cache hit ≈ 50ms. Real networks shift the
absolute numbers; the tier ratios are the point.

## Chunk size (`cache.chunk_size_mb`, default 16)

| Smaller chunks (4–8MB) | Larger chunks (32–64MB) |
|---|---|
| finer on-demand granularity, less over-fetch for small ranges | fewer meta entries, fewer P2P round trips per object |
| more meta/gossip overhead per object | larger granularity of wasted fetch for range reads |
| better load spreading across peers | better sequential throughput per connection |

Fleet rule: **keep chunk size identical on every node** — chunk ids embed
the chunk size (`<key>_<offset/chunk_size>`); mixed values fragment the
shared cache.

## Cache sizing

- Memory cache holds hot chunks; disk tier is the spill area with an
  in-memory index. A chunk promoted from disk is re-cached in memory.
- `memory_cache_size_mb` ≤ ~60% of RAM (the process also holds in-flight
  downloads: up to K × chunk_size per concurrent object).
- An item larger than the memory cache is refused by the LRU but still
  persisted to disk — oversized objects stay fetchable, just never
  memory-resident.
- Eviction is approximate: the 32 coldest entries are scored by
  `1.0·age + 0.5·replicas + 0.3·heat`. Raise the replica weight if you
  want well-replicated chunks evicted first.

## Zero-copy path

Cache hits hand out `shared_ptr<const Chunk>` — no copy from cache to
proxy to P2P serving. Costs that remain:

- one copy when assembling multi-chunk HTTP responses (`response.body`
  concatenation) — full-object GETs are not zero-copy today
- one copy per P2P download into the winning private buffer
- SHA256 per chunk on every store/download (hardware SHA would help)

## P2P racing (`k_value`, default 5)

K peers race; first verified copy wins, losers abort at the next 256KB
slice. Trade-offs:

| K=2–3 | K=5–10 |
|---|---|
| less duplicate traffic | better tail latency under flaky peers |
| fewer chances when top peers are slow | up to K× chunk traffic in the worst case (bounded by stop-on-first-win) |

For LAN clusters 3 is usually enough. Hedged traffic is bounded: losers are
cancelled as soon as a winner verifies, not run to completion.

## Bandwidth limits

`p2p.max_upload_speed_mbps` / `p2p.max_download_speed_mbps`, `0` = unlimited.
The limiter refills per second window and sleeps between 256KB slices — it
throttles average rate, not burst rate. Set upload limits on shared links;
the serving path otherwise saturates the NIC on large chunk hits.

## Concurrency

- Scheduler threads: `p2p.worker_threads` (default = cores). The proxy HTTP
  server runs its own `elio::run` scheduler in addition.
- Blocking legacy calls (control-plane HTTP) go through the scheduler's
  blocking pool, not workers.
- Max HTTP keep-alive per proxy connection and server-level request caps
  are Elio `server_config` defaults (100 req/connection, 10MB request).

## Disk tier

- `initialize_disk_cache()` only indexes files (fast restart; no bulk load).
- `sync_metadata()` writes metadata JSON atomically — call it on shutdown
  if you care about hash metadata surviving restarts.
- Eviction from memory does not delete from disk; disk is pruned only by
  explicit remove or capacity policy.

## What to measure first

1. `X-Cache` / `X-Chunk-Sources` distribution on real traffic
   (P2P share is the win metric)
2. proxy `get_metrics()` hit rate
3. chunk download p99 (transfer stats) vs origin p99
4. eviction rate vs. hit rate (thrashing shows as hit-rate collapse)
