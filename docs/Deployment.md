# Deployment

## Process model

One ElioP2P binary, three deployment shapes:

| Shape | Command | Runs |
|-------|---------|------|
| Edge node (default) | `ElioP2P [options]` | proxy + cache + P2P + control-plane client |
| Control-plane server | `ElioP2P --server` | control-plane registry only |
| Library | link `ElioP2Plib` | components embedded in your own binary (this is how the E2E tools run) |

Ports used by an edge node:

| Port | Config | Purpose | Expose to |
|------|--------|---------|-----------|
| 8080 | `proxy.listen_port` | S3-style HTTP proxy | clients (your apps) |
| 9000 | `p2p.listen_port` | chunk transfer | cluster only |
| 9001 | `p2p.gossip_port` | gossip protocol | cluster only |
| 8082 | `control_plane_server.listen_port` | control-plane API (server shape only) | cluster only |

Firewall rule of thumb: only the proxy port faces applications; everything
else stays inside the cluster security group.

## Single node (evaluation)

```bash
# 1. MinIO as the origin (see docs/Testing.md for full fixture)
export MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin123
./minio server /data --address :9090 &

# 2. One edge node
ELIOP2P_STORAGE_ENDPOINT=http://localhost:9090 \
ELIOP2P_STORAGE_ACCESS_KEY=minioadmin \
ELIOP2P_STORAGE_SECRET_KEY=minioadmin123 \
./build/bin/ElioP2P \
    --proxy-port 8080 --p2p-port 9000 --p2p-gossip-port 9001 \
    --cache-disk-path /var/cache/eliop2p \
    --log-level info

# 3. Use it
curl http://localhost:8080/test-bucket/path/to/object -O
curl -H 'Range: bytes=0-1048575' http://localhost:8080/test-bucket/big.bin
```

Watch the `X-Cache` and `X-Chunk-Sources` response headers to see which
tier served each chunk.

## Multi-node cluster

There is no seed-node requirement: pick a discovery strategy.

### Option A — control-plane discovery (recommended)

```bash
# node-ctrl (control-plane server)
./build/bin/ElioP2P --server --control-server-bind 0.0.0.0 --control-server-port 8082

# each edge node
ELIOP2P_CONTROL_PLANE=10.0.0.10 ELIOP2P_CONTROL_PLANE_PORT=8082 \
ELIOP2P_STORAGE_ENDPOINT=https://s3.amazonaws.com \
ELIOP2P_STORAGE_ACCESS_KEY=... ELIOP2P_STORAGE_SECRET_KEY=... \
./build/bin/ElioP2P --proxy-port 8080 --p2p-port 9000 --p2p-gossip-port 9001
```

Nodes register with the control plane, exchange chunk availability over
gossip, and pull replication commands with ACK.

### Option B — static gossip wiring

For small clusters or air-gapped environments, wire peers manually at
startup (the API is `NodeDiscovery::add_peer`). With the binary, run one
gossip-seed node and point the others at it via the control-plane config
fields or embed `ElioP2Plib` and call `add_peer` during init.

## systemd

```ini
# /etc/systemd/system/eliop2p.service
[Unit]
Description=ElioP2P edge cache node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
Environment=ELIOP2P_STORAGE_ENDPOINT=https://s3.example.com
Environment=ELIOP2P_STORAGE_ACCESS_KEY=changeme
Environment=ELIOP2P_STORAGE_SECRET_KEY=changeme
Environment=ELIOP2P_LOG_LEVEL=info
ExecStart=/usr/local/bin/ElioP2P \
    --proxy-port 8080 --p2p-port 9000 --p2p-gossip-port 9001 \
    --cache-memory-size 8192 --cache-disk-path /var/cache/eliop2p
Restart=on-failure
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Graceful shutdown: the process handles SIGINT/SIGTERM (stop components,
join threads, exit). Give it a few seconds before SIGKILL.

## Kubernetes

```yaml
apiVersion: apps/v1
kind: DaemonSet          # one cache node per machine
metadata:
  name: eliop2p
spec:
  selector:
    matchLabels: { app: eliop2p }
  template:
    metadata:
      labels: { app: eliop2p }
    spec:
      hostNetwork: true   # simplest for the three-port model
      containers:
      - name: eliop2p
        image: eliop2p:latest
        ports:
        - { containerPort: 8080 }   # proxy
        - { containerPort: 9000 }   # chunk transfer
        - { containerPort: 9001 }   # gossip
        env:
        - { name: ELIOP2P_STORAGE_ENDPOINT, value: "https://s3.example.com" }
        - { name: ELIOP2P_CONTROL_PLANE, value: "eliop2p-ctrl" }
        - { name: ELIOP2P_CONTROL_PLANE_PORT, value: "8082" }
        - name: ELIOP2P_STORAGE_ACCESS_KEY
          valueFrom: { secretKeyRef: { name: s3-creds, key: access } }
        - name: ELIOP2P_STORAGE_SECRET_KEY
          valueFrom: { secretKeyRef: { name: s3-creds, key: secret } }
        resources:
          requests: { memory: "2Gi" }
          limits:   { memory: "10Gi" }   # head-room for the memory cache
        volumeMounts:
        - { name: cache, mountPath: /var/cache/eliop2p }
      volumes:
      - name: cache
        hostPath: { path: /var/lib/eliop2p-cache, type: DirectoryOrCreate }
---
apiVersion: v1
kind: Service
metadata:
  name: eliop2p-ctrl
spec:
  selector: { app: eliop2p-ctrl }
  ports: [{ port: 8082 }]
```

Notes:
- Without `hostNetwork`, map all three ports consistently; the gossip and
  chunk ports a node advertises must be the ones peers can reach it on.
- The disk cache survives restarts (hostPath); memory cache does not.

## Production checklist

- [ ] Proxy port reachable from applications; 9000/9001/8082 firewalled to cluster
- [ ] Storage credentials valid and least-privilege (read-only if writes unused)
- [ ] `proxy.allowed_bucket` set if the proxy should not relay arbitrary buckets
- [ ] `--cache-disk-path` on a real volume with enough space; memory cache ≤ 60% RAM
- [ ] `--p2p-max-upload-speed` set on shared networks (default unlimited)
- [ ] Log level `info` or above in production; `debug` is very chatty
- [ ] Clock sync across nodes (SigV4 rejects skewed signatures)
