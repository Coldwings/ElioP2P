# Configuration

Configuration is resolved in this priority order:

```
command-line options  >  environment variables  >  config file  >  defaults
```

Config file formats: INI or JSON (`--config/-c`). Environment variables and
CLI flags cover every operational setting; this page lists both.

## General

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--server` (flag) | — | off | run as control-plane server instead of edge node |
| `--node-id` | `ELIOP2P_NODE_ID` | generated | node identity used in gossip/control plane |
| `--bind-address` | `ELIOP2P_BIND_ADDRESS` | `0.0.0.0` | default bind for listeners |
| `--log-level` | `ELIOP2P_LOG_LEVEL` | `info` | debug / info / warning / error |
| `--log-output` | — | `stdout` | stdout / stderr / file |
| `--log-file` | — | — | path when output=file |

## Cache

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--cache-memory-size` | — | `4096` | memory cache capacity (MB) |
| `--cache-disk-size` | — | `102400` | disk cache budget (MB) |
| `--cache-chunk-size` | — | `16` | chunk size (MB); chunk ids depend on it — keep identical fleet-wide |
| `--cache-disk-path` | — | `/var/cache/eliop2p` | disk cache root (empty = memory only) |
| `--cache-eviction-threshold` | — | `0.8` | start evicting above this usage ratio |
| `--cache-eviction-target` | — | `0.6` | stop evicting at this ratio |

Eviction weights (`CacheConfig` in code): time 1.0, replica 0.5, heat 0.3;
heat thresholds: hot >100 accesses/h, warm >10/h. Eviction samples the 32
coldest entries and picks the worst score (approximate LRU).

## P2P

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--p2p-port` | `ELIOP2P_P2P_PORT` | `9000` | chunk transfer TCP port |
| — | `ELIOP2P_P2P_GOSSIP_PORT` | `9001` | gossip TCP port |
| `--p2p-max-connections` | — | `100` | connection budget |
| `--p2p-max-peers` | — | `50` | peer table size |
| `--p2p-max-upload-speed` | — | `0` | upload limit Mbps (0 = unlimited) |
| `--p2p-max-download-speed` | — | `0` | download limit Mbps (0 = unlimited) |
| `--p2p-selection-k` | — | `5` | peers raced per download |
| `--p2p-gossip-interval` | — | `10` | seconds between state-sync ticks |
| `--p2p-heartbeat-timeout` | — | `60` | seconds before a peer is considered lost |
| `--p2p-transport` | — | `tcp` | `tcp` (rdma reserved) |

## Proxy

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--proxy-port` | `ELIOP2P_PROXY_PORT` | `8080` | HTTP proxy port |
| — | `ELIOP2P_PROXY_ALLOWED_BUCKET` | — | when set, only this bucket is served (others → 403) |

## Control plane (client)

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--control-plane-endpoint` | `ELIOP2P_CONTROL_PLANE` | — | control-plane host |
| `--control-plane-port` | `ELIOP2P_CONTROL_PLANE_PORT` | `8081` | control-plane port |
| `--control-plane-heartbeat` | — | `30` | heartbeat interval (s) |
| `--control-plane-reconnect` | — | `5` | reconnect backoff (s) |
| `--control-plane-enable` (flag) | — | on | enable control-plane integration |

## Control plane (server)

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--control-server-port` | — | `8082` | listen port |
| `--control-server-bind` | — | `0.0.0.0` | bind address |
| `--control-server-timeout` | — | `90` | node heartbeat timeout (s) |
| `--control-server-max-nodes` | — | `1000` | registry capacity |
| `--control-server-min-replicas` | — | `2` | replication lower bound |
| `--control-server-max-replicas` | — | `5` | replication upper bound |

## Storage

| CLI | Env | Default | Meaning |
|-----|-----|---------|---------|
| `--storage-type` | — | `s3` | `s3` or `oss` |
| `--storage-endpoint` | `ELIOP2P_STORAGE_ENDPOINT` | — | origin endpoint; `http(s)://` scheme overrides `--storage-https` |
| `--storage-region` | `ELIOP2P_STORAGE_REGION` | `us-east-1` | signing region |
| `--storage-bucket` | `ELIOP2P_STORAGE_BUCKET` | — | default bucket |
| `--storage-access-key` | `ELIOP2P_STORAGE_ACCESS_KEY` | — | credential (omit for public buckets) |
| `--storage-secret-key` | `ELIOP2P_STORAGE_SECRET_KEY` | — | credential |
| `--storage-https` (flag) | — | on | use TLS when endpoint has no explicit scheme |

When credentials are absent, requests to the origin go unsigned (works with
public buckets and anonymous MinIO access, with a warning in the log).

## Example: full CLI edge node

```bash
./ElioP2P \
  --proxy-port 8080 \
  --p2p-port 9000 \
  --cache-memory-size 8192 --cache-disk-path /var/cache/eliop2p \
  --p2p-selection-k 5 --p2p-max-upload-speed 500 \
  --storage-endpoint https://s3.amazonaws.com --storage-region us-east-1 \
  --storage-access-key AKIA... --storage-secret-key ... \
  --control-plane-endpoint 10.0.0.10 --control-plane-port 8082 \
  --log-level info
```
