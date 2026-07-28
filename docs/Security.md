# Security

## Trust model

**The storage origin is the only trust root.** Peers are not trusted by
default; they are trusted only insofar as their claims can be verified
against origin-anchored data.

```
origin (SigV4-signed channel)
  → first backhaul computes per-chunk SHA256
  → hashes ride along in the "#meta" chunk (size/etag/hash[])
  → every P2P download enforces expected_sha256 when known
```

Two distinct hash checks exist, and they defend different things:

| Check | Where | Defends against |
|-------|-------|-----------------|
| transfer hash (sender-computed) | every Response/Upload message | wire corruption, buggy peer |
| origin-anchored hash (`expected_sha256`) | inside `download_from_peer` | **malicious peer serving wrong bytes with a consistent self-hash** (cache poisoning) |

A peer whose bytes fail the origin-anchored check loses the hedged race
outright; honest peers win instead. Verified end-to-end in
`test/e2e_p2p_cluster.cpp` (evil node serves 16MB of `0xEE` with a
self-consistent hash and still loses).

Until a chunk's hash has been learned from the origin it is *weak-trust*:
the first successful copy is accepted and then anchored. This window exists
once per chunk per node and closes permanently after first verification.

## Attack surface and mitigations

| Vector | Mitigation in code |
|--------|--------------------|
| **Cache poisoning via P2P** | origin-anchored hash verification (above); Upload payloads verified before store |
| **Path traversal via chunk_id** | chunk ids are allowlist-sanitized before use as disk filenames; `..` cannot escape the chunks directory |
| **Open proxy relay** | `proxy.allowed_bucket` restricts service to one bucket (403 otherwise); combine with network ACLs |
| **Gossip spoofing** | content-hash message dedup; chunk claims are only as good as the origin-anchored verification downstream |
| **Credential leakage** | credentials only via env/CLI, never logged; unsigned mode only when credentials absent (with a warning) |
| **SigV4 downgrade** | `http://` endpoints are plaintext by explicit scheme only; `https` by default |
| **Malformed protocol input** | bounded chunk_id length (256B), bounded upload size (64MB), length-checked deserialization, `read_exact` framing |

## Boundaries you must secure in deployment

1. **Ports 9000/9001 (chunk/gossip) and 8082 (control plane)** have *no
   authentication*. They must be reachable only from cluster members
   (security group / firewall). The integrity story above assumes an
   attacker cannot freely join the gossip network; it does not authenticate
   peers.
2. **Proxy port (8080)** has no client authentication by default. Deploy
   behind your app's network boundary, or set `allowed_bucket` plus an
   ACL. A client that can reach the proxy can read any object the node's
   storage credentials can read (for its allowed buckets).
3. **Credentials**: prefer environment variables over CLI flags (flags show
   in `ps`). Use read-only storage credentials if you don't need PUT.
4. **Clock**: SigV4 requires sane clocks; skewed nodes get 403s from S3.

## Reporting

Open an issue at <https://github.com/Coldwings/ElioP2P/issues> or contact
the maintainer directly for sensitive reports.
