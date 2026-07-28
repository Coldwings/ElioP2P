# Protocols

ElioP2P uses three wire protocols. All multi-byte integer fields on the wire
are **network byte order** unless stated otherwise.

## 1. Chunk transfer protocol (port `p2p.listen_port`, default 9000)

Binary request/response over TCP. One connection carries one request and one
response, then closes.

### Header (57 bytes, packed)

| Offset | Field | Size | Notes |
|--------|-------|------|-------|
| 0  | magic           | 4  | `0x43484B50` ("CHKP") |
| 4  | version         | 4  | currently `1` |
| 8  | message_type    | 4  | see below |
| 12 | chunk_id_length | 4  | ≤ 256 |
| 16 | data_length     | 4  | payload bytes after the chunk id |
| 20 | hash[32]        | 32 | SHA256 of the payload (see per-type rules) |
| 52 | sequence_number | 4  | reserved, 0 (see note below) |
| 56 | flags           | 1  | `0x01` RESUME, `0x02` COMPRESSED, `0x04` LAST_PART |

> The packed struct is 57 bytes; fields are converted with
> `header_to_wire()/header_from_wire()` at both ends — never send the raw
> struct in host byte order.

### Message types

| Type | Value | Direction | Payload after chunk id | hash field |
|------|-------|-----------|------------------------|------------|
| Request   | 1 | downloader → server | none (`data_length=0`) | zero |
| Response  | 2 | server → downloader | `data_length` bytes of chunk data | SHA256(data) |
| Error     | 3 | either | none | zero |
| Ack       | 4 | server → uploader | none | SHA256 of received data |
| Upload    | 5 | uploader → server | `data_length` bytes to store | SHA256(data) |

### Exchanges

**Download:** `Request(chunk_id)` → `Response(chunk_id, hash, data)` or
`Error(chunk_id)`. The downloader verifies `SHA256(data) == hash`, and when
`TransferRequest.expected_sha256` is set, additionally enforces the
origin-anchored hash. Either failure discards the peer's result (the peer
loses the hedged race).

**Upload:** `Upload(chunk_id, hash, data)` → receiver verifies
`SHA256(data) == hash`, stores via `chunk_data_consumer`, replies
`Ack(chunk_id, receiver_hash)`. The uploader succeeds only when the Ack
arrives and the echoed hash matches its own.

**Limits:** `chunk_id_length ≤ 256`; upload `data_length ≤ 64MB`
(4 × chunk size). Reads use `read_exact` loops (TCP segmentation is
expected); writes use `write_exact`.

## 2. Gossip protocol (port `p2p.gossip_port`, default 9001)

Length-prefixed binary messages over TCP, one message per connection:

```
[4 bytes: message_size (payload size, not including these 4 bytes)]
[message_size bytes: payload]
```

### Payload layout

| Field | Encoding |
|-------|----------|
| magic        | u32 = `GOSSIP_MAGIC` |
| version      | u32 |
| message_type | u32: 1=NodeJoin 2=NodeLeave 3=ChunkAnnounce 4=ChunkRemove 5=StateSync |
| source_node_id | u32 len + bytes |
| timestamp    | u64 (be64) |
| chunk_map    | u32 count; per entry: u32 id_len + node_id, u32 chunk_count, then per chunk u32 len + bytes |
| peer_updates | u32 count; per entry: u32 id_len + node_id, u32 addr_len + address, u16 port, u16 gossip_port, u64 last_seen, u64 mem_mb, u64 disk_mb, double latency_ms, double throughput_mbps, u32 chunk_count + chunk ids |

`gossip_port = 0` means "same as port". Doubles are raw IEEE-754 host layout
(currently little-endian x86-64 across the fleet; a future version should fix
byte order explicitly).

### Semantics

- **NodeJoin/NodeLeave** — membership changes, merged into the peer table.
- **ChunkAnnounce/ChunkRemove** — `chunk_map[source] +=/−= chunks`; merged
  into the receiver's `chunk_locations` index. Announcements are broadcast
  immediately on change to a bounded random subset; duplicates are dropped
  via a content-hash message id (source + timestamp + FNV-1a of payload),
  so identical rebroadcasts dedupe but distinct payloads never collide.
- **StateSync** — periodic (gossip interval) full-state exchange used for
  anti-entropy: lost announces re-converge. Also refreshes `last_seen`.
- Peers with `last_seen` older than the heartbeat timeout are purged by the
  periodic cleanup.

## 3. Control-plane HTTP API (port `control_plane_server.listen_port`, default 8082)

JSON over HTTP/1.1.

| Method & path | Purpose | Notes |
|---------------|---------|-------|
| `POST /api/v1/nodes` | register node | body: node info incl. available chunks |
| `PUT /api/v1/nodes/:id/heartbeat` | heartbeat | extends liveness window |
| `PUT /api/v1/nodes/:id/chunks` | replace reported chunk set | index updated under one lock |
| `GET  /api/v1/nodes` | list registered nodes | |
| `POST /api/v1/chunks/locations` | locate chunks | body: `{"chunk_ids": [...]}` |
| `GET  /api/v1/nodes/:id/commands` | pull replication commands | delivered ≠ removed (see ACK) |
| `POST /api/v1/nodes/:id/commands/ack` | acknowledge a command | body: `{"command_id": "..."}`; only then is it deleted |
| `GET  /health` | liveness probe | |

**Command delivery semantics:** `GET commands` marks commands delivered but
keeps them; un-ACKed commands are redelivered after 60s. Clients should
process-then-ACK (see [[API Contracts]] for the at-least-once contract).

## Proxy HTTP interface (port `proxy.listen_port`, default 8080)

S3-style `GET/HEAD /bucket/key`. Query strings (incl. presigned URLs) do not
affect the cache key. See [[API Contracts]] for full behavioral rules.

Response headers of note:

- `X-Cache`: `HIT` | `P2P` | `MISS` — dominant tier of the response
- `X-Chunk-Sources`: e.g. `0:CACHE,1:P2P,2:STORAGE` — per-chunk provenance
- `Accept-Ranges: bytes`, `ETag`, and for 206 `Content-Range`
