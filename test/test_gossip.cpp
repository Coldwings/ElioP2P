#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

TEST_CASE("Gossip Message Types", "[gossip][enum]") {
    // Test all message types
    REQUIRE(static_cast<int>(GossipMessageType::NodeJoin) == 0);
    REQUIRE(static_cast<int>(GossipMessageType::NodeLeave) == 1);
    REQUIRE(static_cast<int>(GossipMessageType::ChunkAnnounce) == 2);
    REQUIRE(static_cast<int>(GossipMessageType::ChunkRemove) == 3);
    REQUIRE(static_cast<int>(GossipMessageType::StateSync) == 4);
}

TEST_CASE("Node Registration", "[gossip][node]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);

    // Start discovery
    REQUIRE(discovery.start() == true);

    // Register a local node
    PeerNode local_node;
    local_node.node_id = "test_node_1";
    local_node.address = "192.168.1.1";
    local_node.port = 9000;
    local_node.available_memory_mb = 4096;
    local_node.available_disk_mb = 102400;

    REQUIRE(discovery.register_node(local_node) == true);

    // Verify node is registered
    auto peers = discovery.get_all_peers();
    REQUIRE(peers.size() == 1);
    REQUIRE(peers[0].node_id == "test_node_1");

    discovery.stop();
}

TEST_CASE("Chunk Announcement", "[gossip][chunk]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Register a local node
    PeerNode local_node;
    local_node.node_id = "test_node_2";
    local_node.address = "192.168.1.2";
    local_node.port = 9001;
    discovery.register_node(local_node);

    // Announce a chunk
    std::string chunk_id = "test_object_0_chunk_0";
    discovery.announce_chunk(chunk_id);

    // Verify chunk is announced
    auto peers_with_chunk = discovery.get_peers_with_chunk(chunk_id);
    REQUIRE(peers_with_chunk.size() == 1);
    REQUIRE(peers_with_chunk[0].node_id == "test_node_2");

    discovery.stop();
}

TEST_CASE("Peer Discovery", "[gossip][peer]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Register local node
    PeerNode local_node;
    local_node.node_id = "test_node_3";
    local_node.address = "192.168.1.3";
    local_node.port = 9002;
    discovery.register_node(local_node);

    // Add another peer manually
    PeerNode remote_peer;
    remote_peer.node_id = "peer_node_1";
    remote_peer.address = "192.168.1.100";
    remote_peer.port = 9000;
    remote_peer.available_memory_mb = 4096;
    remote_peer.available_disk_mb = 102400;
    // Set last_seen to current time so peer is considered active
    remote_peer.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    discovery.add_peer(remote_peer);

    // Verify peer is discovered
    auto all_peers = discovery.get_all_peers();
    REQUIRE(all_peers.size() == 2);

    discovery.stop();
}

TEST_CASE("Gossip Message Creation", "[gossip][message]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Register local node
    PeerNode local_node;
    local_node.node_id = "test_node_4";
    local_node.address = "192.168.1.4";
    local_node.port = 9003;
    discovery.register_node(local_node);

    // Announce some chunks
    discovery.announce_chunk("chunk_1");
    discovery.announce_chunk("chunk_2");
    discovery.announce_chunk("chunk_3");

    // Create state sync message
    auto msg = discovery.create_gossip_message(GossipMessageType::StateSync);

    REQUIRE(msg.type == GossipMessageType::StateSync);
    REQUIRE(msg.source_node_id == "test_node_4");
    REQUIRE(msg.chunk_map.size() >= 3);

    INFO("Message contains " << msg.chunk_map.size() << " chunk entries");

    discovery.stop();
}

TEST_CASE("Gossip Message Handling", "[gossip][message][handler]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Register local node
    PeerNode local_node;
    local_node.node_id = "test_node_5";
    local_node.address = "192.168.1.5";
    local_node.port = 9004;
    discovery.register_node(local_node);

    // Create a gossip message simulating another node
    GossipMessage msg;
    msg.type = GossipMessageType::NodeJoin;
    msg.source_node_id = "new_peer";
    msg.timestamp = 1234567890;

    PeerNode new_peer;
    new_peer.node_id = "new_peer";
    new_peer.address = "192.168.1.10";
    new_peer.port = 9005;
    msg.peer_updates["new_peer"] = new_peer;

    // Handle the message
    discovery.handle_gossip_message(msg);

    // Verify peer was added
    auto all_peers = discovery.get_all_peers();
    REQUIRE(all_peers.size() >= 2);

    discovery.stop();
}

TEST_CASE("Chunk Rarity", "[gossip][rarity]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Register local node
    PeerNode local_node;
    local_node.node_id = "test_node_6";
    local_node.address = "192.168.1.6";
    local_node.port = 9006;
    discovery.register_node(local_node);

    // Add two peers
    PeerNode peer1;
    peer1.node_id = "peer_1";
    peer1.address = "192.168.1.10";
    peer1.port = 9000;
    peer1.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    discovery.add_peer(peer1);

    PeerNode peer2;
    peer2.node_id = "peer_2";
    peer2.address = "192.168.1.11";
    peer2.port = 9000;
    peer2.last_seen = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    discovery.add_peer(peer2);

    // Announce a chunk from local node
    discovery.announce_chunk("rare_chunk");

    // Check rarity - only local node has announced
    uint32_t rarity = discovery.get_chunk_rarity("rare_chunk");
    INFO("Rarity of rare_chunk: " << rarity);
    REQUIRE(rarity >= 1);  // Local node has it

    // Announce another chunk from local node
    discovery.announce_chunk("common_chunk");

    uint32_t common_rarity = discovery.get_chunk_rarity("common_chunk");
    INFO("Rarity of common_chunk: " << common_rarity);
    REQUIRE(common_rarity >= 1);  // Local node has it

    discovery.stop();
}

TEST_CASE("Discovery Mode Switching", "[gossip][mode]") {
    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Initially should be in ControlPlane mode
    REQUIRE(discovery.get_discovery_mode() == DiscoveryMode::ControlPlane);

    // Switch to gossip-only mode
    discovery.switch_to_gossip_only();
    REQUIRE(discovery.get_discovery_mode() == DiscoveryMode::GossipOnly);

    discovery.stop();
}
