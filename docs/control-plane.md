# ElioP2P Control Plane

The Control Plane is a separate service that needs to be deployed independently from ElioP2P nodes.

## Architecture

```
+-------------------+       +-------------------+
|  ElioP2P Node 1 |       |  ElioP2P Node 2 |
+--------+----------+       +--------+----------+
         |                          |
         +----+                   +----+
              |                   |
         +----v-------------------v----+
         |    Control Plane Service   |
         |    (Sharded + Single)     |
         +---------------------------+
```

## Control Plane Responsibilities

- Node registration and heartbeat management
- Chunk location index
- Replication adjustment decisions
- Command distribution

## Deployment

The control plane is a separate service that must be deployed before ElioP2P nodes. It exposes HTTP API on port 8081 (default).

### API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | /health | Health check |
| POST | /api/v1/nodes | Node registration |
| PUT | /api/v1/nodes/{node_id}/heartbeat | Heartbeat |
| PUT | /api/v1/nodes/{node_id}/chunks | Report chunks |
| POST | /api/v1/chunks/locations | Query chunk locations |
| GET | /api/v1/nodes | List nodes |
| GET | /api/v1/metrics | Get cluster metrics |
| GET | /api/v1/nodes/{node_id}/commands | Get replication commands |

### Running the Control Plane

The control plane service is not yet implemented in this repository. For testing purposes:

1. **Mock/Debug Mode**: ElioP2P nodes can run in Gossip-only mode without a control plane
2. **External Implementation**: A separate control plane service needs to be developed

### Gossip-Only Mode

When the control plane is unavailable, nodes automatically switch to Gossip-only discovery mode:

```
ELIOP2P_CONTROL_PLANE=localhost:8081 ./ElioP2P
```

If the control plane is unreachable, the node will log a warning and continue operating using peer-to-peer discovery.

## Fallback Strategy

When the control plane fails:
- Nodes can still communicate via P2P
- Cache service continues working
- New replication adjustments cannot be initiated
- Metadata can be recovered from nodes via Gossip
