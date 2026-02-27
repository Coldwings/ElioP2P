#ifndef ELIOP2P_CONTROL_CLIENT_H
#define ELIOP2P_CONTROL_CLIENT_H

#include "eliop2p/base/config.h"
#include "eliop2p/p2p/node_discovery.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <elio/elio.hpp>

namespace eliop2p {

struct ChunkLocation {
    std::string chunk_id;
    std::vector<std::string> peer_ids;
};

struct NodeRegistration {
    std::string node_id;
    std::string address;
    uint16_t p2p_port;
    uint16_t http_port;
    uint64_t memory_capacity_mb;
    uint64_t disk_capacity_mb;
    std::vector<std::string> available_chunks;
};

struct NodeStatus {
    std::string node_id;
    bool online = true;
    uint64_t memory_used_mb = 0;
    uint64_t disk_used_mb = 0;
    float cache_hit_rate = 0.0f;
    uint64_t total_chunks = 0;
    uint64_t active_connections = 0;
    double upload_speed_mbps = 0.0;
    double download_speed_mbps = 0.0;
    uint64_t last_update_time = 0;
};

struct ControlPlaneMetrics {
    uint64_t total_nodes = 0;
    uint64_t active_nodes = 0;
    uint64_t total_chunks = 0;
    uint64_t cache_hit_rate = 0;
};

struct ReplicationCommand {
    std::string command_id;
    std::string chunk_id;
    std::string action;  // "replicate", "evict", "migrate"
    std::string source_node_id;
    std::string target_node_id;
    uint32_t priority = 0;
    uint64_t created_at = 0;
};

using ChunkLocationsCallback = std::function<void(const std::vector<ChunkLocation>&)>;
using ReplicationCommandCallback = std::function<void(const std::vector<ReplicationCommand>&)>;

class ControlPlaneClient {
public:
    ControlPlaneClient(const ControlPlaneConfig& config);
    ~ControlPlaneClient();

    // Connect to control plane
    bool connect();

    // Disconnect
    void disconnect();

    // Register this node
    bool register_node(const NodeRegistration& registration);

    // Update node status (heartbeat)
    bool send_heartbeat(const NodeStatus& status);

    // Report available chunks
    bool report_chunks(const std::string& node_id, const std::vector<std::string>& chunk_ids);

    // Query chunk locations
    elio::coro::task<std::optional<std::vector<ChunkLocation>>> query_chunk_locations(
        const std::vector<std::string>& chunk_ids);

    // Query all nodes
    elio::coro::task<std::optional<std::vector<NodeRegistration>>> query_nodes();

    // Subscribe to chunk location updates
    void subscribe_to_updates(ChunkLocationsCallback callback);

    // Subscribe to replication commands
    void subscribe_to_replication_commands(ReplicationCommandCallback callback);

    // Get cluster metrics
    elio::coro::task<std::optional<ControlPlaneMetrics>> get_metrics();

    // Check if connected
    bool is_connected() const;

    // Start heartbeat loop (runs in background)
    void start_heartbeat_loop(const std::function<NodeStatus()>& status_provider);

    // Stop heartbeat loop
    void stop_heartbeat_loop();

    // Check if heartbeat is running
    bool is_heartbeat_running() const;

    // Get pending replication commands
    elio::coro::task<std::optional<std::vector<ReplicationCommand>>> fetch_replication_commands();

    // Set scheduler for coroutines (for global scheduler integration)
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_CONTROL_CLIENT_H
