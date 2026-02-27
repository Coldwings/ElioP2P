#ifndef ELIOP2P_CONTROL_SERVER_H
#define ELIOP2P_CONTROL_SERVER_H

#include "eliop2p/base/config.h"
#include "eliop2p/control/metadata.h"
#include "eliop2p/control/client.h"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <elio/elio.hpp>

namespace eliop2p {

// Server-specific metrics
struct ControlPlaneServerMetrics {
    uint64_t total_nodes = 0;
    uint64_t active_nodes = 0;
    uint64_t total_heartbeats = 0;
    uint64_t total_chunk_reports = 0;
    uint64_t total_queries = 0;
    uint64_t commands_issued = 0;
    uint64_t replication_decisions = 0;
};

class ControlPlaneServer {
public:
    ControlPlaneServer(const ControlPlaneServerConfig& config);
    ~ControlPlaneServer();

    // Start the control plane server
    bool start();

    // Stop the control plane server
    void stop();

    // Check if server is running
    bool is_running() const;

    // Get server metrics (thread-safe copy)
    ControlPlaneServerMetrics get_metrics() const;

    // Get listen port (useful if port was assigned dynamically)
    uint16_t get_listen_port() const;

    // Get number of registered nodes
    size_t get_node_count() const;

    // Get number of active nodes
    size_t get_active_node_count() const;

    // Query all registered nodes
    std::vector<NodeRegistration> get_all_nodes() const;

    // Query chunk locations (for API)
    std::optional<std::vector<ChunkLocation>> query_chunk_locations(
        const std::vector<std::string>& chunk_ids) const;

    // Set scheduler for coroutines (for global scheduler integration)
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_CONTROL_SERVER_H
