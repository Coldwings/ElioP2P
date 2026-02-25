# ElioP2P Configuration Guide

## Configuration Methods

ElioP2P supports configuration via:
1. Configuration file (INI format)
2. Environment variables
3. Command line arguments

Priority: Command line > Environment variables > Config file > Defaults

## Configuration File

Default path: `/etc/eliop2p/config.ini`

Example:
```ini
[general]
node_id = node_001
log_level = INFO

[cache]
memory_cache_size_mb = 4096
disk_cache_size_mb = 102400
chunk_size_mb = 16

[p2p]
listen_port = 9000
max_peers = 50

[proxy]
listen_port = 8080

[control_plane]
endpoint = localhost
port = 8081

[storage]
endpoint = http://localhost:9000
region = us-east-1
```

## Environment Variables

### General

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_NODE_ID | Node identifier | Generated |
| ELIOP2P_BIND_ADDRESS | Bind address | 0.0.0.0 |
| ELIOP2P_LOG_LEVEL | Log level | INFO |

### Cache

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_CACHE_MEMORY_SIZE | Memory cache size (MB) | 4096 |
| ELIOP2P_CACHE_DISK_SIZE | Disk cache size (MB) | 102400 |
| ELIOP2P_CACHE_CHUNK_SIZE | Chunk size (MB) | 16 |

### P2P

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_P2P_PORT | P2P listen port | 9000 |
| ELIOP2P_P2P_MAX_PEERS | Maximum peers | 50 |
| ELIOP2P_P2P_TIMEOUT | Connection timeout (ms) | 30000 |

### Proxy

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_PROXY_PORT | HTTP proxy port | 8080 |
| ELIOP2P_PROXY_BIND | Proxy bind address | 0.0.0.0 |

### Control Plane

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_CONTROL_PLANE | Control plane host | localhost |
| ELIOP2P_CONTROL_PLANE_PORT | Control plane port | 8081 |

### Storage

| Variable | Description | Default |
|----------|-------------|---------|
| ELIOP2P_STORAGE_ENDPOINT | Storage endpoint | - |
| ELIOP2P_STORAGE_REGION | Storage region | us-east-1 |
| ELIOP2P_STORAGE_BUCKET | Default bucket | - |

## Command Line Options

```
ElioP2P [OPTIONS]

Options:
  --help, -h           Show help message
  --version, -v        Show version
  --config, -c         Config file path

Examples:
  ElioP2P --config /etc/eliop2p/config.ini
  ELIOP2P_STORAGE_ENDPOINT=http://s3.amazonaws.com ./ElioP2P
```

## Cache Configuration

### Memory Cache

- **memory_cache_size_mb**: Memory cache size (MB)
- Range: 1024 - 65536 (1GB - 64GB)

### Disk Cache

- **disk_cache_size_mb**: Disk cache size (MB)
- Range: 10240 - 524288 (10GB - 512GB)
- Default path: /var/cache/eliop2p/chunks

### Chunk Configuration

- **chunk_size_mb**: Chunk size in MB
- Default: 16MB (fixed)

### Eviction

- **eviction_threshold**: Trigger eviction at capacity %
- Default: 0.8 (80%)

- **eviction_target**: Stop eviction at capacity %
- Default: 0.6 (60%)

- **eviction_weights**:
  - w1 (time): 1.0
  - w2 (replicas): 0.5
  - w3 (heat): 0.3

## P2P Configuration

### Network

- **listen_port**: P2P communication port
- Default: 9000

- **max_peers**: Maximum number of peers
- Default: 50

### Bandwidth

- **download_limit_mbps**: Max download speed (Mbps)
- Default: 0 (unlimited)

- **upload_limit_mbps**: Max upload speed (Mbps)
- Default: 0 (unlimited)

### Transfer

- **k_selection**: Number of peers to download from
- Default: 5

- **checkpoint_interval_mb**: Checkpoint interval
- Default: 1MB

## Storage Configuration

### S3

```ini
[storage]
endpoint = http://s3.amazonaws.com
region = us-east-1
access_key = your_access_key
secret_key = your_secret_key
```

### OSS

```ini
[storage]
endpoint = http://oss-cn-shanghai.aliyuncs.com
region = cn-shanghai
access_key = your_access_key
secret_key = your_secret_key
bucket = your_bucket
```

## Kubernetes Deployment

### Port Requirements

| Port | Protocol | Purpose |
|------|----------|---------|
| 8080 | TCP | HTTP Proxy |
| 9000 | TCP | P2P |

### Example Deployment

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: eliop2p
spec:
  containers:
  - name: eliop2p
    image: eliop2p:latest
    ports:
    - containerPort: 8080
      protocol: TCP
    - containerPort: 9000
      protocol: TCP
    env:
    - name: ELIOP2P_STORAGE_ENDPOINT
      value: "http://s3.amazonaws.com"
    - name: ELIOP2P_LOG_LEVEL
      value: "INFO"
    volumeMounts:
    - name: cache
      mountPath: /var/cache/eliop2p
  volumes:
  - name: cache
    emptyDir:
      sizeLimit: 100Gi
```
