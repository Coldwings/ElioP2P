#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/config.h"
#include <elio/net/tcp.hpp>
#include <chrono>
#include <random>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <arpa/inet.h>

namespace eliop2p {

// Gossip protocol constants
static const uint32_t GOSSIP_PROTOCOL_VERSION = 1;
static const uint32_t GOSSIP_MAGIC = 0x47505350;  // "GPSP"
static const size_t MAX_MESSAGE_SIZE = 64 * 1024;  // 64KB max message size

// Implementation details
struct NodeDiscovery::Impl {
    P2PConfig config;
    std::unordered_map<std::string, PeerNode> peers;
    std::unordered_map<std::string, std::unordered_set<std::string>> chunk_locations;
    std::string local_node_id;
    std::string local_node_address;
    uint16_t local_node_port = 0;
    uint16_t listen_port = 0;
    bool running = false;
    DiscoveryMode mode = DiscoveryMode::ControlPlane;

    NodeDiscoveredCallback on_node_discovered;
    NodeLostCallback on_node_lost;

    // Gossip protocol state
    std::atomic<bool> gossip_running{false};
    std::vector<elio::coro::task<void>> gossip_tasks;
    std::unordered_set<std::string> gossip_neighbors;
    uint64_t gossip_interval_ms = 10000;  // 10 seconds
    uint64_t heartbeat_interval_ms = 5000;  // 5 seconds
    std::mt19937 rng_{std::random_device{}()};

    // Message deduplication
    std::unordered_set<std::string> received_messages;
    std::chrono::steady_clock::time_point last_cleanup;

    // Control plane connection state
    std::atomic<bool> control_plane_available{true};
    uint64_t last_heartbeat_time = 0;
    std::chrono::steady_clock::time_point last_mode_switch;

    // TCP server state
    std::shared_ptr<elio::runtime::scheduler> scheduler;
    std::atomic<bool> tcp_server_running{false};
    std::thread tcp_server_thread;
    std::optional<elio::net::tcp_listener> tcp_listener;
    std::atomic<bool> server_stopped{true};

    Impl(const P2PConfig& cfg) : config(cfg) {
        gossip_interval_ms = cfg.gossip_interval_sec * 1000;
        heartbeat_interval_ms = cfg.heartbeat_timeout_sec * 1000 / 12;  // More frequent heartbeats
        last_cleanup = std::chrono::steady_clock::now();
    }

    // Generate message ID for deduplication
    std::string generate_message_id(const GossipMessage& msg) {
        std::stringstream ss;
        ss << msg.source_node_id << ":" << msg.timestamp << ":" << static_cast<int>(msg.type);
        return ss.str();
    }

    // Check if message was already received
    bool is_message_seen(const GossipMessage& msg) {
        std::string id = generate_message_id(msg);
        return received_messages.find(id) != received_messages.end();
    }

    // Mark message as seen
    void mark_message_seen(const GossipMessage& msg) {
        std::string id = generate_message_id(msg);
        received_messages.insert(id);

        // Periodic cleanup of old message IDs
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now - last_cleanup).count() >= 5) {
            received_messages.clear();
            last_cleanup = now;
        }
    }

    // Clean up stale peers
    void cleanup_stale_peers() {
        std::vector<std::string> to_remove;

        for (const auto& [id, peer] : peers) {
            if (!peer.is_active()) {
                to_remove.push_back(id);
            }
        }

        for (const auto& id : to_remove) {
            peers.erase(id);
            // Also remove from chunk locations
            for (auto& [chunk, locs] : chunk_locations) {
                locs.erase(id);
            }
            // Notify listener
            Logger::instance().debug("Removed stale peer: " + id);
        }
    }

    // Select random peers for gossip
    std::vector<PeerNode> select_gossip_targets(uint32_t count) {
        std::vector<PeerNode> targets;
        auto all_peers = get_all_peers_internal();

        if (all_peers.empty()) {
            return targets;
        }

        // Shuffle and take first 'count' peers
        std::shuffle(all_peers.begin(), all_peers.end(), rng_);
        uint32_t actual_count = std::min(count, static_cast<uint32_t>(all_peers.size()));
        targets.assign(all_peers.begin(), all_peers.begin() + actual_count);

        return targets;
    }

    // Internal get all peers (without cleanup)
    std::vector<PeerNode> get_all_peers_internal() const {
        std::vector<PeerNode> result;
        for (const auto& [id, peer] : peers) {
            if (peer.is_active()) {
                result.push_back(peer);
            }
        }
        return result;
    }

    // TCP server accept loop coroutine
    elio::coro::task<void> tcp_server_accept_loop() {
        if (!tcp_listener) {
            co_return;
        }

        Logger::instance().info("TCP gossip server accept loop started");

        while (tcp_server_running.load()) {
            auto stream_result = co_await tcp_listener->accept();

            if (!stream_result) {
                if (tcp_server_running.load()) {
                    Logger::instance().error("Accept error: " + std::string(strerror(errno)));
                }
                continue;
            }

            // Spawn handler coroutine
            auto handler = handle_tcp_connection(std::move(*stream_result));
            scheduler->spawn(handler.release());
        }

        Logger::instance().info("TCP gossip server accept loop stopped");
    }

    // Start TCP server in a separate thread
    void start_tcp_server_internal(uint16_t port) {
        if (tcp_server_running.load()) {
            Logger::instance().warning("TCP server already running");
            return;
        }

        tcp_server_running = true;
        server_stopped = false;

        tcp_server_thread = std::thread([this, port]() {
            Logger::instance().info("Starting TCP gossip server on port " + std::to_string(port));

            // Create scheduler for TCP server
            scheduler = std::make_shared<elio::runtime::scheduler>(2);
            scheduler->start();

            // Bind TCP listener
            elio::net::tcp_options opts;
            opts.reuse_addr = true;
            opts.no_delay = true;
            opts.backlog = 64;

            auto bind_addr = elio::net::socket_address(elio::net::ipv4_address(port));
            auto listener_result = elio::net::tcp_listener::bind(bind_addr, opts);

            if (!listener_result) {
                Logger::instance().error("Failed to bind TCP listener: " + std::string(strerror(errno)));
                tcp_server_running = false;
                server_stopped = true;
                scheduler->shutdown();
                return;
            }

            tcp_listener = std::move(*listener_result);
            listen_port = tcp_listener->local_address().port();
            Logger::instance().info("TCP gossip server listening on port " + std::to_string(listen_port));

            // Update local node port if needed
            if (local_node_port == 0) {
                local_node_port = listen_port;
            }

            // Start accept loop coroutine
            auto accept_loop = tcp_server_accept_loop();
            scheduler->spawn(accept_loop.release());

            // Keep thread alive while running
            while (tcp_server_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Close listener
            if (tcp_listener) {
                tcp_listener->close();
            }

            server_stopped = true;
            Logger::instance().info("TCP gossip server stopped");
        });
    }

    // Handle incoming TCP connection
    elio::coro::task<void> handle_tcp_connection(elio::net::tcp_stream stream) {
        auto peer = stream.peer_address();
        Logger::instance().debug("Handling TCP connection from " +
                               (peer ? peer->to_string() : "unknown"));

        char buffer[MAX_MESSAGE_SIZE];

        try {
            // Read message size (4 bytes)
            auto header_result = co_await stream.read(buffer, 4);
            if (header_result.result <= 0) {
                co_return;
            }

            // Parse message size
            uint32_t msg_size = 0;
            std::memcpy(&msg_size, buffer, 4);
            msg_size = ntohl(msg_size);

            if (msg_size > MAX_MESSAGE_SIZE || msg_size == 0) {
                Logger::instance().warning("Invalid message size: " + std::to_string(msg_size));
                co_return;
            }

            // Read full message
            size_t total_read = 0;
            while (total_read < msg_size) {
                auto result = co_await stream.read(buffer + total_read, msg_size - total_read);
                if (result.result <= 0) {
                    co_return;
                }
                total_read += result.result;
            }

            // Deserialize message
            std::string msg_data(buffer, msg_size);
            auto msg_opt = deserialize_gossip_message(msg_data);

            if (!msg_opt) {
                Logger::instance().warning("Failed to deserialize gossip message");
                co_return;
            }

            // Handle the message (this will be called on NodeDiscovery)
            // Note: We need to pass this back to the main handler

        } catch (const std::exception& e) {
            Logger::instance().error("TCP connection handler error: " + std::string(e.what()));
        }

        co_return;
    }

    // Deserialize gossip message from binary format
    std::optional<GossipMessage> deserialize_gossip_message(const std::string& data) {
        if (data.size() < 12) {  // Minimum: magic(4) + version(4) + type(4)
            return std::nullopt;
        }

        size_t offset = 0;

        // Check magic
        uint32_t magic = 0;
        std::memcpy(&magic, data.data() + offset, 4);
        magic = ntohl(magic);
        offset += 4;

        if (magic != GOSSIP_MAGIC) {
            Logger::instance().warning("Invalid gossip magic: " + std::to_string(magic));
            return std::nullopt;
        }

        // Check version
        uint32_t version = 0;
        std::memcpy(&version, data.data() + offset, 4);
        version = ntohl(version);
        offset += 4;

        if (version != GOSSIP_PROTOCOL_VERSION) {
            Logger::instance().warning("Unsupported gossip version: " + std::to_string(version));
            return std::nullopt;
        }

        // Read message type
        uint32_t msg_type = 0;
        std::memcpy(&msg_type, data.data() + offset, 4);
        msg_type = ntohl(msg_type);
        offset += 4;

        GossipMessage msg;
        msg.type = static_cast<GossipMessageType>(msg_type);

        // Read source node ID
        uint32_t node_id_len = 0;
        std::memcpy(&node_id_len, data.data() + offset, 4);
        node_id_len = ntohl(node_id_len);
        offset += 4;

        if (offset + node_id_len > data.size()) return std::nullopt;
        msg.source_node_id = data.substr(offset, node_id_len);
        offset += node_id_len;

        // Read timestamp
        uint64_t timestamp = 0;
        std::memcpy(&timestamp, data.data() + offset, 8);
        timestamp = be64toh(timestamp);
        offset += 8;

        msg.timestamp = timestamp;

        // Read chunk map
        uint32_t chunk_map_size = 0;
        std::memcpy(&chunk_map_size, data.data() + offset, 4);
        chunk_map_size = ntohl(chunk_map_size);
        offset += 4;

        for (uint32_t i = 0; i < chunk_map_size; ++i) {
            // Read node ID
            uint32_t node_id_len = 0;
            std::memcpy(&node_id_len, data.data() + offset, 4);
            node_id_len = ntohl(node_id_len);
            offset += 4;

            if (offset + node_id_len > data.size()) return std::nullopt;
            std::string node_id = data.substr(offset, node_id_len);
            offset += node_id_len;

            // Read chunks
            uint32_t chunk_count = 0;
            std::memcpy(&chunk_count, data.data() + offset, 4);
            chunk_count = ntohl(chunk_count);
            offset += 4;

            std::unordered_set<std::string> chunks;
            for (uint32_t j = 0; j < chunk_count; ++j) {
                uint32_t chunk_len = 0;
                std::memcpy(&chunk_len, data.data() + offset, 4);
                chunk_len = ntohl(chunk_len);
                offset += 4;

                if (offset + chunk_len > data.size()) return std::nullopt;
                chunks.insert(data.substr(offset, chunk_len));
                offset += chunk_len;
            }

            msg.chunk_map[node_id] = std::move(chunks);
        }

        // Read peer updates
        uint32_t peer_count = 0;
        std::memcpy(&peer_count, data.data() + offset, 4);
        peer_count = ntohl(peer_count);
        offset += 4;

        for (uint32_t i = 0; i < peer_count; ++i) {
            // Read node ID
            uint32_t node_id_len = 0;
            std::memcpy(&node_id_len, data.data() + offset, 4);
            node_id_len = ntohl(node_id_len);
            offset += 4;

            if (offset + node_id_len > data.size()) return std::nullopt;
            std::string node_id = data.substr(offset, node_id_len);
            offset += node_id_len;

            // Read peer info
            PeerNode peer;
            peer.node_id = node_id;

            // Read address
            uint32_t addr_len = 0;
            std::memcpy(&addr_len, data.data() + offset, 4);
            addr_len = ntohl(addr_len);
            offset += 4;

            if (offset + addr_len > data.size()) return std::nullopt;
            peer.address = data.substr(offset, addr_len);
            offset += addr_len;

            // Read port
            uint16_t port = 0;
            std::memcpy(&port, data.data() + offset, 2);
            port = ntohs(port);
            offset += 2;
            peer.port = port;

            // Read last_seen
            uint64_t last_seen = 0;
            std::memcpy(&last_seen, data.data() + offset, 8);
            last_seen = be64toh(last_seen);
            offset += 8;
            peer.last_seen = last_seen;

            // Read available_memory_mb
            uint64_t mem_mb = 0;
            std::memcpy(&mem_mb, data.data() + offset, 8);
            mem_mb = be64toh(mem_mb);
            offset += 8;
            peer.available_memory_mb = mem_mb;

            // Read available_disk_mb
            uint64_t disk_mb = 0;
            std::memcpy(&disk_mb, data.data() + offset, 8);
            disk_mb = be64toh(disk_mb);
            offset += 8;
            peer.available_disk_mb = disk_mb;

            // Read latency_ms
            double latency = 0.0;
            std::memcpy(&latency, data.data() + offset, sizeof(double));
            offset += sizeof(double);
            peer.latency_ms = latency;

            // Read throughput_mbps
            double throughput = 0.0;
            std::memcpy(&throughput, data.data() + offset, sizeof(double));
            offset += sizeof(double);
            peer.throughput_mbps = throughput;

            // Read chunk count
            uint32_t chunk_count = 0;
            std::memcpy(&chunk_count, data.data() + offset, 4);
            chunk_count = ntohl(chunk_count);
            offset += 4;

            for (uint32_t j = 0; j < chunk_count; ++j) {
                uint32_t chunk_len = 0;
                std::memcpy(&chunk_len, data.data() + offset, 4);
                chunk_len = ntohl(chunk_len);
                offset += 4;

                if (offset + chunk_len > data.size()) return std::nullopt;
                peer.available_chunks.insert(data.substr(offset, chunk_len));
                offset += chunk_len;
            }

            msg.peer_updates[node_id] = std::move(peer);
        }

        return msg;
    }

    // Serialize gossip message to binary format
    std::string serialize_gossip_message(const GossipMessage& msg) {
        std::string data;
        data.reserve(256);  // Pre-allocate for typical message

        // Write magic
        uint32_t magic = htonl(GOSSIP_MAGIC);
        data.append(reinterpret_cast<const char*>(&magic), 4);

        // Write version
        uint32_t version = htonl(GOSSIP_PROTOCOL_VERSION);
        data.append(reinterpret_cast<const char*>(&version), 4);

        // Write message type
        uint32_t msg_type = htonl(static_cast<uint32_t>(msg.type));
        data.append(reinterpret_cast<const char*>(&msg_type), 4);

        // Write source node ID
        uint32_t node_id_len = htonl(static_cast<uint32_t>(msg.source_node_id.size()));
        data.append(reinterpret_cast<const char*>(&node_id_len), 4);
        data.append(msg.source_node_id);

        // Write timestamp
        uint64_t timestamp = htobe64(msg.timestamp);
        data.append(reinterpret_cast<const char*>(&timestamp), 8);

        // Write chunk map
        uint32_t chunk_map_size = htonl(static_cast<uint32_t>(msg.chunk_map.size()));
        data.append(reinterpret_cast<const char*>(&chunk_map_size), 4);

        for (const auto& [node_id, chunks] : msg.chunk_map) {
            // Write node ID
            uint32_t nid_len = htonl(static_cast<uint32_t>(node_id.size()));
            data.append(reinterpret_cast<const char*>(&nid_len), 4);
            data.append(node_id);

            // Write chunk count
            uint32_t chunk_count = htonl(static_cast<uint32_t>(chunks.size()));
            data.append(reinterpret_cast<const char*>(&chunk_count), 4);

            // Write each chunk
            for (const auto& chunk : chunks) {
                uint32_t chunk_len = htonl(static_cast<uint32_t>(chunk.size()));
                data.append(reinterpret_cast<const char*>(&chunk_len), 4);
                data.append(chunk);
            }
        }

        // Write peer updates
        uint32_t peer_count = htonl(static_cast<uint32_t>(msg.peer_updates.size()));
        data.append(reinterpret_cast<const char*>(&peer_count), 4);

        for (const auto& [node_id, peer] : msg.peer_updates) {
            // Write node ID
            uint32_t nid_len = htonl(static_cast<uint32_t>(node_id.size()));
            data.append(reinterpret_cast<const char*>(&nid_len), 4);
            data.append(node_id);

            // Write address
            uint32_t addr_len = htonl(static_cast<uint32_t>(peer.address.size()));
            data.append(reinterpret_cast<const char*>(&addr_len), 4);
            data.append(peer.address);

            // Write port
            uint16_t port = htons(peer.port);
            data.append(reinterpret_cast<const char*>(&port), 2);

            // Write last_seen
            uint64_t last_seen = htobe64(peer.last_seen);
            data.append(reinterpret_cast<const char*>(&last_seen), 8);

            // Write available_memory_mb
            uint64_t mem_mb = htobe64(peer.available_memory_mb);
            data.append(reinterpret_cast<const char*>(&mem_mb), 8);

            // Write available_disk_mb
            uint64_t disk_mb = htobe64(peer.available_disk_mb);
            data.append(reinterpret_cast<const char*>(&disk_mb), 8);

            // Write latency_ms
            double latency = peer.latency_ms;
            data.append(reinterpret_cast<const char*>(&latency), sizeof(double));

            // Write throughput_mbps
            double throughput = peer.throughput_mbps;
            data.append(reinterpret_cast<const char*>(&throughput), sizeof(double));

            // Write chunk count
            uint32_t chunk_count = htonl(static_cast<uint32_t>(peer.available_chunks.size()));
            data.append(reinterpret_cast<const char*>(&chunk_count), 4);

            // Write each chunk
            for (const auto& chunk : peer.available_chunks) {
                uint32_t chunk_len = htonl(static_cast<uint32_t>(chunk.size()));
                data.append(reinterpret_cast<const char*>(&chunk_len), 4);
                data.append(chunk);
            }
        }

        return data;
    }

    // Send gossip message via TCP to a peer
    elio::coro::task<bool> send_gossip_message_to_peer(const PeerNode& peer, const GossipMessage& msg) {
        if (peer.node_id == local_node_id) {
            co_return true;  // Don't send to self
        }

        try {
            // Connect to peer
            elio::net::tcp_options opts;
            opts.no_delay = true;

            auto connect_result = co_await elio::net::tcp_connect(peer.address, peer.port, opts);

            if (!connect_result) {
                Logger::instance().warning("Failed to connect to peer " + peer.node_id +
                                          " at " + peer.address + ":" + std::to_string(peer.port) +
                                          ": " + std::string(strerror(errno)));
                co_return false;
            }

            auto& stream = *connect_result;

            // Serialize message
            std::string data = serialize_gossip_message(msg);

            // Send message size first (4 bytes, network byte order)
            uint32_t msg_size = htonl(static_cast<uint32_t>(data.size()));
            std::string size_header(reinterpret_cast<const char*>(&msg_size), 4);

            auto write_result = co_await stream.write(size_header.data(), size_header.size());
            if (write_result.result != static_cast<ssize_t>(size_header.size())) {
                Logger::instance().warning("Failed to send message size to peer " + peer.node_id);
                co_return false;
            }

            // Send message data
            auto data_result = co_await stream.write(data.data(), data.size());
            if (data_result.result != static_cast<ssize_t>(data.size())) {
                Logger::instance().warning("Failed to send message data to peer " + peer.node_id);
                co_return false;
            }

            Logger::instance().debug("Sent gossip message to " + peer.node_id +
                                    " (" + peer.address + ":" + std::to_string(peer.port) + ")");

            // Close the stream
            co_await stream.close();

            co_return true;

        } catch (const std::exception& e) {
            Logger::instance().error("Exception sending gossip to peer " + peer.node_id +
                                     ": " + std::string(e.what()));
            co_return false;
        }
    }

    void stop_tcp_server_internal() {
        if (!tcp_server_running.load()) {
            return;
        }

        Logger::instance().info("Stopping TCP gossip server");

        tcp_server_running = false;

        // Close the listener to interrupt pending accepts
        if (tcp_listener) {
            tcp_listener->close();
        }

        // Wait for thread to finish
        if (tcp_server_thread.joinable()) {
            tcp_server_thread.join();
        }

        // Shutdown scheduler
        if (scheduler) {
            scheduler->shutdown();
            scheduler.reset();
        }

        Logger::instance().info("TCP gossip server stopped");
    }
};

bool PeerNode::is_active() const {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - last_seen) < 300;  // 5 minutes timeout
}

NodeDiscovery::NodeDiscovery(const P2PConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

NodeDiscovery::~NodeDiscovery() {
    stop();
}

bool NodeDiscovery::start() {
    impl_->running = true;
    Logger::instance().info("Node discovery started in ControlPlane mode");
    return true;
}

void NodeDiscovery::stop() {
    impl_->running = false;
    stop_gossip_protocol();
    stop_tcp_server();
    Logger::instance().info("Node discovery stopped");
}

bool NodeDiscovery::register_node(const PeerNode& local_node) {
    impl_->local_node_id = local_node.node_id;
    impl_->local_node_address = local_node.address;
    impl_->local_node_port = local_node.port;

    // Register local node as a peer
    PeerNode local_peer = local_node;
    local_peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    impl_->peers[local_peer.node_id] = local_peer;

    // Announce local chunks
    for (const auto& [chunk, locs] : impl_->chunk_locations) {
        (void)locs;  // suppress unused warning
    }

    Logger::instance().info("Registered node: " + local_node.node_id +
                           " at " + local_node.address + ":" + std::to_string(local_node.port));

    // Start TCP server for receiving gossip messages
    start_tcp_server();

    // Start gossip protocol
    start_gossip_protocol();

    // Broadcast node join to neighbors
    broadcast_node_join();

    return true;
}

void NodeDiscovery::unregister_node() {
    if (!impl_->local_node_id.empty()) {
        // Broadcast node leave to neighbors
        broadcast_node_leave();

        Logger::instance().info("Unregistered node: " + impl_->local_node_id);
        impl_->local_node_id.clear();
        impl_->local_node_address.clear();
        impl_->local_node_port = 0;
    }
    stop_gossip_protocol();
}

PeerList NodeDiscovery::get_all_peers() const {
    PeerList result;
    impl_->cleanup_stale_peers();

    for (const auto& [id, peer] : impl_->peers) {
        if (peer.is_active()) {
            result.push_back(peer);
        }
    }
    return result;
}

PeerList NodeDiscovery::get_peers_with_chunk(const std::string& chunk_id) const {
    PeerList result;
    auto it = impl_->chunk_locations.find(chunk_id);
    if (it == impl_->chunk_locations.end()) {
        return result;
    }

    for (const auto& peer_id : it->second) {
        auto peer_it = impl_->peers.find(peer_id);
        if (peer_it != impl_->peers.end() && peer_it->second.is_active()) {
            result.push_back(peer_it->second);
        }
    }
    return result;
}

void NodeDiscovery::announce_chunk(const std::string& chunk_id) {
    if (impl_->local_node_id.empty()) return;
    impl_->chunk_locations[chunk_id].insert(impl_->local_node_id);

    // Update local peer's chunk list
    auto& peer = impl_->peers[impl_->local_node_id];
    peer.available_chunks.insert(chunk_id);

    Logger::instance().debug("Announced chunk: " + chunk_id);

    // Broadcast chunk announcement to neighbors
    broadcast_chunk_announce(chunk_id);
}

void NodeDiscovery::remove_chunk_announcement(const std::string& chunk_id) {
    auto it = impl_->chunk_locations.find(chunk_id);
    if (it != impl_->chunk_locations.end()) {
        it->second.erase(impl_->local_node_id);
    }

    // Update local peer's chunk list
    auto peer_it = impl_->peers.find(impl_->local_node_id);
    if (peer_it != impl_->peers.end()) {
        peer_it->second.available_chunks.erase(chunk_id);
    }

    // Broadcast chunk removal to neighbors
    broadcast_chunk_remove(chunk_id);
}

void NodeDiscovery::set_on_node_discovered(NodeDiscoveredCallback callback) {
    impl_->on_node_discovered = std::move(callback);
}

void NodeDiscovery::set_on_node_lost(NodeLostCallback callback) {
    impl_->on_node_lost = std::move(callback);
}

void NodeDiscovery::add_peer(const PeerNode& peer) {
    bool is_new = impl_->peers.find(peer.node_id) == impl_->peers.end();
    impl_->peers[peer.node_id] = peer;

    if (is_new && impl_->on_node_discovered) {
        impl_->on_node_discovered(peer);
    }
}

void NodeDiscovery::remove_peer(const std::string& node_id) {
    auto it = impl_->peers.find(node_id);
    if (it != impl_->peers.end()) {
        impl_->peers.erase(it);
        // Also remove from chunk locations
        for (auto& [chunk, locs] : impl_->chunk_locations) {
            locs.erase(node_id);
        }
        if (impl_->on_node_lost) {
            impl_->on_node_lost(node_id);
        }
    }
}

DiscoveryMode NodeDiscovery::get_discovery_mode() const {
    return impl_->mode;
}

void NodeDiscovery::switch_to_gossip_only() {
    if (impl_->mode != DiscoveryMode::GossipOnly) {
        impl_->mode = DiscoveryMode::GossipOnly;
        impl_->control_plane_available = false;
        impl_->last_mode_switch = std::chrono::steady_clock::now();
        Logger::instance().warning("Switched to Gossip-only discovery mode (control plane unavailable)");
    }
}

// Gossip protocol implementation
void NodeDiscovery::start_gossip_protocol() {
    if (impl_->gossip_running.load()) {
        return;
    }

    impl_->gossip_running = true;
    Logger::instance().info("Starting gossip protocol with interval: " +
                           std::to_string(impl_->gossip_interval_ms) + "ms");
}

void NodeDiscovery::broadcast_node_join() {
    if (impl_->local_node_id.empty() || !impl_->gossip_running.load()) {
        return;
    }

    // Create node join message
    GossipMessage join_msg;
    join_msg.type = GossipMessageType::NodeJoin;
    join_msg.source_node_id = impl_->local_node_id;
    join_msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Include local node info
    PeerNode local_peer;
    local_peer.node_id = impl_->local_node_id;
    local_peer.address = impl_->local_node_address;
    local_peer.port = impl_->local_node_port;
    local_peer.last_seen = join_msg.timestamp / 1000;
    join_msg.peer_updates[impl_->local_node_id] = local_peer;

    // Get all active peers and broadcast
    auto peers = get_all_peers();
    uint32_t target_count = std::min(impl_->config.max_peers / 2, static_cast<uint32_t>(peers.size()));

    // Randomly select peers to send to
    std::shuffle(peers.begin(), peers.end(), impl_->rng_);

    // Spawn async sends to all targets
    for (uint32_t i = 0; i < target_count && i < peers.size(); ++i) {
        if (peers[i].node_id != impl_->local_node_id) {
            Logger::instance().debug("Broadcasting NodeJoin to: " + peers[i].node_id);

            // Send via TCP - spawn coroutine if we have a scheduler
            if (impl_->scheduler) {
                auto send_task = impl_->send_gossip_message_to_peer(peers[i], join_msg);
                impl_->scheduler->spawn(send_task.release());
            }
        }
    }
}

void NodeDiscovery::broadcast_node_leave() {
    if (impl_->local_node_id.empty()) {
        return;
    }

    // Create node leave message
    GossipMessage leave_msg;
    leave_msg.type = GossipMessageType::NodeLeave;
    leave_msg.source_node_id = impl_->local_node_id;
    leave_msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Get all active peers and broadcast
    auto peers = get_all_peers();
    uint32_t target_count = std::min(impl_->config.max_peers / 2, static_cast<uint32_t>(peers.size()));

    std::shuffle(peers.begin(), peers.end(), impl_->rng_);

    for (uint32_t i = 0; i < target_count && i < peers.size(); ++i) {
        if (peers[i].node_id != impl_->local_node_id) {
            Logger::instance().debug("Broadcasting NodeLeave to: " + peers[i].node_id);

            // Send via TCP
            if (impl_->scheduler) {
                auto send_task = impl_->send_gossip_message_to_peer(peers[i], leave_msg);
                impl_->scheduler->spawn(send_task.release());
            }
        }
    }
}

void NodeDiscovery::broadcast_chunk_announce(const std::string& chunk_id) {
    if (impl_->local_node_id.empty() || !impl_->gossip_running.load()) {
        return;
    }

    GossipMessage msg;
    msg.type = GossipMessageType::ChunkAnnounce;
    msg.source_node_id = impl_->local_node_id;
    msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Add chunk to chunk_map
    msg.chunk_map[impl_->local_node_id].insert(chunk_id);

    // Broadcast to random subset of peers
    auto peers = get_all_peers();
    uint32_t target_count = std::min(impl_->config.max_peers / 3, static_cast<uint32_t>(peers.size()));

    std::shuffle(peers.begin(), peers.end(), impl_->rng_);

    for (uint32_t i = 0; i < target_count && i < peers.size(); ++i) {
        Logger::instance().debug("Broadcasting ChunkAnnounce for " + chunk_id + " to: " + peers[i].node_id);

        // Send via TCP
        if (impl_->scheduler) {
            auto send_task = impl_->send_gossip_message_to_peer(peers[i], msg);
            impl_->scheduler->spawn(send_task.release());
        }
    }
}

void NodeDiscovery::broadcast_chunk_remove(const std::string& chunk_id) {
    if (impl_->local_node_id.empty() || !impl_->gossip_running.load()) {
        return;
    }

    GossipMessage msg;
    msg.type = GossipMessageType::ChunkRemove;
    msg.source_node_id = impl_->local_node_id;
    msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Broadcast to random subset of peers
    auto peers = get_all_peers();
    uint32_t target_count = std::min(impl_->config.max_peers / 3, static_cast<uint32_t>(peers.size()));

    std::shuffle(peers.begin(), peers.end(), impl_->rng_);

    for (uint32_t i = 0; i < target_count && i < peers.size(); ++i) {
        Logger::instance().debug("Broadcasting ChunkRemove for " + chunk_id + " to: " + peers[i].node_id);

        // Send via TCP
        if (impl_->scheduler) {
            auto send_task = impl_->send_gossip_message_to_peer(peers[i], msg);
            impl_->scheduler->spawn(send_task.release());
        }
    }
}

void NodeDiscovery::stop_gossip_protocol() {
    impl_->gossip_running = false;
    Logger::instance().info("Stopping gossip protocol");
}

elio::coro::task<void> NodeDiscovery::gossip_with_peer(const PeerNode& peer) {
    if (!impl_->running || !impl_->gossip_running.load()) {
        co_return;
    }

    if (peer.node_id == impl_->local_node_id) {
        co_return;
    }

    // Create gossip message with state sync
    GossipMessage msg = create_gossip_message(GossipMessageType::StateSync);

    Logger::instance().debug("Gossip with peer: " + peer.node_id +
                           ", peers: " + std::to_string(msg.peer_updates.size()) +
                           ", chunks: " + std::to_string(msg.chunk_map.size()));

    // Send via TCP
    bool success = co_await impl_->send_gossip_message_to_peer(peer, msg);

    if (!success) {
        Logger::instance().debug("Failed to send gossip to peer: " + peer.node_id);
    }

    // Update peer's last seen time
    auto it = impl_->peers.find(peer.node_id);
    if (it != impl_->peers.end()) {
        it->second.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

GossipMessage NodeDiscovery::create_gossip_message(GossipMessageType type) {
    GossipMessage msg;
    msg.type = type;
    msg.source_node_id = impl_->local_node_id;
    msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Include current chunk locations
    msg.chunk_map = impl_->chunk_locations;

    // Include peer information for state sync
    if (type == GossipMessageType::StateSync) {
        for (const auto& [id, peer] : impl_->peers) {
            if (peer.is_active()) {
                msg.peer_updates[id] = peer;
            }
        }
    } else if (type == GossipMessageType::NodeJoin || type == GossipMessageType::NodeLeave) {
        // For join/leave, only include local node info
        auto it = impl_->peers.find(impl_->local_node_id);
        if (it != impl_->peers.end()) {
            msg.peer_updates[impl_->local_node_id] = it->second;
        }
    }

    return msg;
}

void NodeDiscovery::handle_gossip_message(const GossipMessage& msg) {
    // Check for message deduplication
    if (impl_->is_message_seen(msg)) {
        Logger::instance().debug("Ignoring duplicate gossip message from: " + msg.source_node_id);
        return;
    }
    impl_->mark_message_seen(msg);

    Logger::instance().debug("Handling gossip message type: " +
                           std::to_string(static_cast<int>(msg.type)) +
                           " from: " + msg.source_node_id);

    switch (msg.type) {
        case GossipMessageType::NodeJoin:
            handle_node_join(msg);
            break;
        case GossipMessageType::NodeLeave:
            handle_node_leave(msg);
            break;
        case GossipMessageType::ChunkAnnounce:
            handle_chunk_announce(msg);
            break;
        case GossipMessageType::ChunkRemove:
            handle_chunk_remove(msg);
            break;
        case GossipMessageType::StateSync:
            handle_state_sync(msg);
            break;
    }
}

void NodeDiscovery::handle_node_join(const GossipMessage& msg) {
    Logger::instance().info("Node joined via gossip: " + msg.source_node_id);

    // Update peer information
    for (const auto& [peer_id, peer] : msg.peer_updates) {
        if (peer_id != impl_->local_node_id) {
            bool is_new = impl_->peers.find(peer_id) == impl_->peers.end();
            impl_->peers[peer_id] = peer;

            if (is_new && impl_->on_node_discovered) {
                impl_->on_node_discovered(peer);
                Logger::instance().info("New peer discovered via gossip: " + peer_id);
            }
        }
    }
}

void NodeDiscovery::handle_node_leave(const GossipMessage& msg) {
    Logger::instance().info("Node left via gossip: " + msg.source_node_id);

    // Remove the peer
    auto it = impl_->peers.find(msg.source_node_id);
    if (it != impl_->peers.end()) {
        impl_->peers.erase(it);

        // Remove from chunk locations
        for (auto& [chunk, locs] : impl_->chunk_locations) {
            locs.erase(msg.source_node_id);
        }

        if (impl_->on_node_lost) {
            impl_->on_node_lost(msg.source_node_id);
        }
    }
}

void NodeDiscovery::handle_chunk_announce(const GossipMessage& msg) {
    // Update chunk locations
    for (const auto& [node_id, chunks] : msg.chunk_map) {
        if (node_id != impl_->local_node_id) {
            for (const auto& chunk : chunks) {
                impl_->chunk_locations[chunk].insert(node_id);
                Logger::instance().debug("Chunk announced: " + chunk + " from node: " + node_id);
            }

            // Update peer's available chunks
            auto peer_it = impl_->peers.find(node_id);
            if (peer_it != impl_->peers.end()) {
                peer_it->second.available_chunks.insert(chunks.begin(), chunks.end());
            }
        }
    }
}

void NodeDiscovery::handle_chunk_remove(const GossipMessage& msg) {
    // Remove chunk locations
    for (const auto& [node_id, chunks] : msg.chunk_map) {
        if (node_id != impl_->local_node_id) {
            for (const auto& chunk : chunks) {
                auto chunk_it = impl_->chunk_locations.find(chunk);
                if (chunk_it != impl_->chunk_locations.end()) {
                    chunk_it->second.erase(node_id);
                    Logger::instance().debug("Chunk removed: " + chunk + " from node: " + node_id);
                }
            }

            // Update peer's available chunks
            auto peer_it = impl_->peers.find(node_id);
            if (peer_it != impl_->peers.end()) {
                for (const auto& chunk : chunks) {
                    peer_it->second.available_chunks.erase(chunk);
                }
            }
        }
    }
}

void NodeDiscovery::handle_state_sync(const GossipMessage& msg) {
    // Update peer information
    for (const auto& [peer_id, peer] : msg.peer_updates) {
        if (peer_id != impl_->local_node_id) {
            bool is_new = impl_->peers.find(peer_id) == impl_->peers.end();
            impl_->peers[peer_id] = peer;

            if (is_new && impl_->on_node_discovered) {
                impl_->on_node_discovered(peer);
            }
        }
    }

    // Update chunk locations
    for (const auto& [node_id, chunks] : msg.chunk_map) {
        if (node_id != impl_->local_node_id) {
            for (const auto& chunk : chunks) {
                impl_->chunk_locations[chunk].insert(node_id);
            }
        }
    }

    // Update peer's available chunks
    auto peer_it = impl_->peers.find(msg.source_node_id);
    if (peer_it != impl_->peers.end()) {
        for (const auto& [node_id, chunks] : msg.chunk_map) {
            if (node_id == msg.source_node_id) {
                peer_it->second.available_chunks = chunks;
                break;
            }
        }
    }
}

elio::coro::task<void> NodeDiscovery::gossip_tick() {
    if (!impl_->running || !impl_->gossip_running.load()) {
        co_return;
    }

    // Get all active peers for gossip
    auto peers = get_all_peers();

    // Limit number of peers to gossip with (random subset)
    uint32_t max_gossip = std::min(static_cast<uint32_t>(peers.size()),
                                   impl_->config.max_peers / 2);

    std::shuffle(peers.begin(), peers.end(), impl_->rng_);

    for (uint32_t i = 0; i < max_gossip && i < peers.size(); ++i) {
        co_await gossip_with_peer(peers[i]);
    }

    // Clean up stale peers periodically
    impl_->cleanup_stale_peers();
}

elio::coro::task<void> NodeDiscovery::heartbeat_tick() {
    if (!impl_->running) {
        co_return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - impl_->last_mode_switch).count();

    // Check if we should try to reconnect to control plane
    if (impl_->mode == DiscoveryMode::GossipOnly && elapsed >= 60) {
        // In a real implementation, try to reconnect to control plane
        // For now, we'll stay in gossip-only mode
        Logger::instance().debug("Still in gossip-only mode, waiting for control plane recovery");
    }

    // Update local node heartbeat
    if (!impl_->local_node_id.empty()) {
        auto& peer = impl_->peers[impl_->local_node_id];
        peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

void NodeDiscovery::update_peer_latency(const std::string& node_id, double latency_ms) {
    auto it = impl_->peers.find(node_id);
    if (it != impl_->peers.end()) {
        // Exponential moving average
        if (it->second.latency_ms == 0.0) {
            it->second.latency_ms = latency_ms;
        } else {
            it->second.latency_ms = 0.7 * it->second.latency_ms + 0.3 * latency_ms;
        }
    }
}

void NodeDiscovery::update_peer_throughput(const std::string& node_id, double throughput_mbps) {
    auto it = impl_->peers.find(node_id);
    if (it != impl_->peers.end()) {
        // Exponential moving average
        if (it->second.throughput_mbps == 0.0) {
            it->second.throughput_mbps = throughput_mbps;
        } else {
            it->second.throughput_mbps = 0.7 * it->second.throughput_mbps + 0.3 * throughput_mbps;
        }
    }
}

uint32_t NodeDiscovery::get_chunk_rarity(const std::string& chunk_id) const {
    auto it = impl_->chunk_locations.find(chunk_id);
    if (it == impl_->chunk_locations.end()) {
        return 0;  // No peers have this chunk
    }
    return static_cast<uint32_t>(it->second.size());
}

// TCP Server implementation
uint16_t NodeDiscovery::get_listen_port() const {
    return impl_->listen_port;
}

elio::coro::task<void> NodeDiscovery::start_tcp_server() {
    uint16_t port = impl_->config.listen_port;
    if (port == 0) {
        port = 9000;  // Default port
    }

    impl_->start_tcp_server_internal(port);

    co_return;
}

void NodeDiscovery::stop_tcp_server() {
    impl_->stop_tcp_server_internal();
}

std::shared_ptr<elio::runtime::scheduler> NodeDiscovery::get_scheduler() const {
    return impl_->scheduler;
}

// Private methods for TCP communication
elio::coro::task<bool> NodeDiscovery::send_gossip_message(const PeerNode& peer, const GossipMessage& msg) {
    co_return co_await impl_->send_gossip_message_to_peer(peer, msg);
}

elio::coro::task<std::optional<GossipMessage>> NodeDiscovery::receive_gossip_message(
    elio::net::tcp_stream& stream) {
    char buffer[MAX_MESSAGE_SIZE];

    // Read header
    auto header_result = co_await stream.read(buffer, 4);
    if (header_result.result <= 0) {
        co_return std::nullopt;
    }

    uint32_t msg_size = 0;
    std::memcpy(&msg_size, buffer, 4);
    msg_size = ntohl(msg_size);

    if (msg_size > MAX_MESSAGE_SIZE || msg_size == 0) {
        co_return std::nullopt;
    }

    // Read message
    size_t total_read = 0;
    while (total_read < msg_size) {
        auto result = co_await stream.read(buffer + total_read, msg_size - total_read);
        if (result.result <= 0) {
            co_return std::nullopt;
        }
        total_read += result.result;
    }

    std::string msg_data(buffer, msg_size);
    co_return impl_->deserialize_gossip_message(msg_data);
}

std::string NodeDiscovery::serialize_gossip_message(const GossipMessage& msg) {
    return impl_->serialize_gossip_message(msg);
}

std::optional<GossipMessage> NodeDiscovery::deserialize_gossip_message(const std::string& data) {
    return impl_->deserialize_gossip_message(data);
}

} // namespace eliop2p
