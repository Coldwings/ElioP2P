#ifndef ELIOP2P_P2P_NODE_DISCOVERY_H
#define ELIOP2P_P2P_NODE_DISCOVERY_H

#include "eliop2p/base/config.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <chrono>
#include <elio/elio.hpp>
#include <optional>

namespace eliop2p {

struct PeerNode {
    std::string node_id;
    std::string address;
    uint16_t port;
    uint64_t last_seen;
    uint64_t available_memory_mb;
    uint64_t available_disk_mb;
    double latency_ms = 0.0;           // Estimated latency to this peer
    double throughput_mbps = 0.0;      // Measured throughput from this peer
    bool is_active() const;

    // For Rarest-First selection
    std::unordered_set<std::string> available_chunks;
};

using PeerList = std::vector<PeerNode>;
using NodeDiscoveredCallback = std::function<void(const PeerNode&)>;
using NodeLostCallback = std::function<void(const std::string& node_id)>;

// Gossip message types
enum class GossipMessageType {
    NodeJoin,
    NodeLeave,
    ChunkAnnounce,
    ChunkRemove,
    StateSync
};

struct GossipMessage {
    GossipMessageType type;
    std::string source_node_id;
    uint64_t timestamp;
    std::unordered_map<std::string, std::unordered_set<std::string>> chunk_map;  // node_id -> chunks
    std::unordered_map<std::string, PeerNode> peer_updates;
};

// Discovery mode
enum class DiscoveryMode {
    ControlPlane,    // Using control plane for discovery
    GossipOnly,      // Fallback to gossip-only mode
    Hybrid           // Using both
};

class NodeDiscovery {
public:
    NodeDiscovery(const P2PConfig& config);
    ~NodeDiscovery();

    // Start discovery service
    bool start();

    // Stop discovery service
    void stop();

    // Register local node with control plane
    bool register_node(const PeerNode& local_node);

    // Unregister local node
    void unregister_node();

    // Get all known peers
    PeerList get_all_peers() const;

    // Get peers that have a specific chunk
    PeerList get_peers_with_chunk(const std::string& chunk_id) const;

    // Announce local chunk availability
    void announce_chunk(const std::string& chunk_id);

    // Remove chunk announcement
    void remove_chunk_announcement(const std::string& chunk_id);

    // Set callbacks
    void set_on_node_discovered(NodeDiscoveredCallback callback);
    void set_on_node_lost(NodeLostCallback callback);

    // Manual peer addition (for testing)
    void add_peer(const PeerNode& peer);

    // Remove peer
    void remove_peer(const std::string& node_id);

    // Gossip protocol methods
    void start_gossip_protocol();
    void stop_gossip_protocol();
    elio::coro::task<void> gossip_with_peer(const PeerNode& peer);
    GossipMessage create_gossip_message(GossipMessageType type);
    void handle_gossip_message(const GossipMessage& msg);

    // Message handlers for different gossip types
    void handle_node_join(const GossipMessage& msg);
    void handle_node_leave(const GossipMessage& msg);
    void handle_chunk_announce(const GossipMessage& msg);
    void handle_chunk_remove(const GossipMessage& msg);
    void handle_state_sync(const GossipMessage& msg);

    // Broadcast methods
    void broadcast_node_join();
    void broadcast_node_leave();
    void broadcast_chunk_announce(const std::string& chunk_id);
    void broadcast_chunk_remove(const std::string& chunk_id);

    // Get current discovery mode
    DiscoveryMode get_discovery_mode() const;

    // Switch to gossip-only mode (control plane unavailable)
    void switch_to_gossip_only();

    // Periodic gossip tick (called by internal timer)
    elio::coro::task<void> gossip_tick();

    // Update peer latency
    void update_peer_latency(const std::string& node_id, double latency_ms);

    // Update peer throughput
    void update_peer_throughput(const std::string& node_id, double throughput_mbps);

    // Get chunk rarity (how few peers have this chunk)
    uint32_t get_chunk_rarity(const std::string& chunk_id) const;

    // Periodic heartbeat (called by internal timer)
    elio::coro::task<void> heartbeat_tick();

    // TCP server methods
    uint16_t get_listen_port() const;
    elio::coro::task<void> start_tcp_server();
    void stop_tcp_server();

    // Get scheduler for spawning coroutines
    std::shared_ptr<elio::runtime::scheduler> get_scheduler() const;

    // Set scheduler (for global scheduler integration)
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Internal TCP communication methods
    elio::coro::task<bool> send_gossip_message(const PeerNode& peer, const GossipMessage& msg);
    elio::coro::task<std::optional<GossipMessage>> receive_gossip_message(
        elio::net::tcp_stream& stream);
    std::string serialize_gossip_message(const GossipMessage& msg);
    std::optional<GossipMessage> deserialize_gossip_message(const std::string& data);
};

} // namespace eliop2p

#endif // ELIOP2P_P2P_NODE_DISCOVERY_H
