# Testing

## Unit tests (ctest)

```bash
cmake --build build -j
cd build && ctest --output-on-failure
```

70 Catch2 cases across cache, gossip, transfer, storage, control plane,
and proxy layers.

## End-to-end fixtures (real MinIO + real nodes)

The E2E tools are **not** registered with ctest — they bind real ports and
need a live MinIO. Run them explicitly.

### 1. MinIO fixture

```bash
# binary (no docker needed)
curl -L -o /tmp/minio https://dl.min.io/server/minio/release/linux-amd64/minio
chmod +x /tmp/minio
MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin123 \
    /tmp/minio server /tmp/minio-data --address :9090 &

# client + bucket + test objects
curl -L -o /tmp/mc https://dl.min.io/client/mc/release/linux-amd64/mc && chmod +x /tmp/mc
/tmp/mc alias set local http://localhost:9090 minioadmin minioadmin123
/tmp/mc mb local/test-bucket

# deterministic test data (byte patterns make corruption easy to spot)
python3 - <<'EOF'
import os
os.makedirs('/tmp/testdata/photos/2024', exist_ok=True)
for name, size in [('small.txt', 1024),
                   ('photos/2024/medium_20mb.bin', 20*1024*1024),
                   ('photos/2024/large_40mb.bin', 40*1024*1024)]:
    p = f'/tmp/testdata/{name}'
    with open(p, 'wb') as f:
        blk = bytearray(1024*1024)
        for i in range(size // (1024*1024) + 1):
            blk[0:8] = i.to_bytes(8, 'little')
            f.write(blk[:min(len(blk), size - i*1024*1024)])
EOF
/tmp/mc cp --recursive /tmp/testdata/ local/test-bucket/
```

### 2. `e2e_p2p_cluster` — distribution & integrity (22 checks)

Two full nodes with gossip wiring. Node A's storage endpoint points at a
**dead port**: anything it serves provably came over P2P.

- multi-segment S3 keys route correctly
- 20MB object byte-identical to origin from both nodes
- meta + data chunks distribute over gossip
- Range: intra-chunk, cross-chunk, suffix (206 + exact bytes)
- **active attack**: evil node C announces chunk0 and serves 16MB of
  `0xEE` with a self-consistent hash — the origin-anchored check must
  make it lose, and A must receive origin bytes anyway

```bash
./build/bin/e2e_p2p_cluster    # exit 0 when all 22 pass
```

### 3. `e2e_p2p_partial` — on-demand partial reads (22 checks)

Proves the "no complete copy anywhere, yet everything reads" scenario:
B pre-warms only chunk0; A mixes `0:P2P`, `1:STORAGE`, `0:CACHE,1:CACHE`,
and a full-object `0:CACHE,1:CACHE,2:STORAGE` response; A never
over-fetches (chunk2 stays absent until read); B later pulls chunk1
**from A** (reverse distribution). Per-chunk provenance is asserted from
the `X-Chunk-Sources` header.

```bash
./build/bin/e2e_p2p_partial    # exit 0 when all 22 pass
```

### 4. Debugging probes

- `sig_probe` — one signed GET against the configured endpoint; prints the
  canonical request on failure. Use when a backend returns 403.
- `head_probe` — same for HEAD requests / multi-segment keys.

## Writing new tests

- Unit: Catch2 in `test/`, registered in `test/CMakeLists.txt`.
- E2E: prefer the `TestNode` pattern from `e2e_p2p_cluster.cpp` — real
  components, real ports, assertions on wire-visible behavior
  (status codes, `X-Chunk-Sources`, SHA256). Avoid mocking the layers you
  want to trust.
