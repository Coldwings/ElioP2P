#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

void test_gossip_message_types() {
    std::cout << "Testing gossip message types..." << std::endl;

    // Test all message types
    assert(static_cast<int>(GossipMessageType::NodeJoin) == 0);
    assert(static_cast<int>(GossipMessageType::NodeLeave) == 1);
    assert(static_cast<int>(GossipMessageType::ChunkAnnounce) == 2);
    assert(static_cast<int>(GossipMessageType::ChunkRemove) == 3);
    assert(static_cast<int>(GossipMessageType::StateSync) == 4);

    std::cout << "  Gossip message types test PASSED" << std::endl;
}

void test_node_registration() {
    std::cout << "Testing node registration..." << std::endl;

    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);

    // Start discovery
    assert(discovery.start());

    // Register a local node
    PeerNode local_node;
    local_node.node_id = "test_node_1";
    local_node.address = "192.168.1.1";
    local_node.port = 9000;
    local_node.available_memory_mb = 4096;
    local_node.available_disk_mb = 102400;

    assert(discovery.register_node(local_node));

    // Verify node is registered
    auto peers = discovery.get_all_peers();
    assert(peers.size() == 1);
    assert(peers[0].node_id == "test_node_1");

    std::cout << "  Node registration test PASSED" << std::endl;

    discovery.stop();
}

void test_chunk_announcement() {
    std::cout << "Testing chunk announcement..." << std::endl;

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
    assert(peers_with_chunk.size() == 1);
    assert(peers_with_chunk[0].node_id == "test_node_2");

    std::cout << "  Chunk announcement test PASSED" << std::endl;

    discovery.stop();
}

void test_peer_discovery() {
    std::cout << "Testing peer discovery..." << std::endl;

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
    discovery.add_peer(remote_peer);

    // Verify peer is discovered
    auto all_peers = discovery.get_all_peers();
    assert(all_peers.size() == 2);

    std::cout << "  Peer discovery test PASSED" << std::endl;

    discovery.stop();
}

void test_gossip_message_creation() {
    std::cout << "Testing gossip message creation..." << std::endl;

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

    assert(msg.type == GossipMessageType::StateSync);
    assert(msg.source_node_id == "test_node_4");
    assert(msg.chunk_map.size() >= 3);

    std::cout << "  Message contains " << msg.chunk_map.size() << " chunk entries" << std::endl;
    std::cout << "  Gossip message creation test PASSED" << std::endl;

    discovery.stop();
}

void test_gossip_message_handling() {
    std::cout << "Testing gossip message handling..." << std::endl;

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
    assert(all_peers.size() >= 2);

    std::cout << "  Gossip message handling test PASSED" << std::endl;

    discovery.stop();
}

void test_chunk_rarity() {
    std::cout << "Testing chunk rarity..." << std::endl;

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
    discovery.add_peer(peer1);

    PeerNode peer2;
    peer2.node_id = "peer_2";
    peer2.address = "192.168.1.11";
    peer2.port = 9000;
    discovery.add_peer(peer2);

    // Announce a chunk from peer1 only
    discovery.announce_chunk("rare_chunk");

    // Check rarity
    uint32_t rarity = discovery.get_chunk_rarity("rare_chunk");
    std::cout << "  Rarity of rare_chunk: " << rarity << std::endl;
    assert(rarity >= 1);  // Should have at least local node + peer1

    // Announce another chunk from both peers
    discovery.announce_chunk("common_chunk");

    uint32_t common_rarity = discovery.get_chunk_rarity("common_chunk");
    std::cout << "  Rarity of common_chunk: " << common_rarity << std::endl;
    assert(common_rarity >= 2);

    std::cout << "  Chunk rarity test PASSED" << std::endl;

    discovery.stop();
}

void test_discovery_mode_switching() {
    std::cout << "Testing discovery mode switching..." << std::endl;

    P2PConfig config;
    config.max_peers = 10;
    config.gossip_interval_sec = 10;

    NodeDiscovery discovery(config);
    discovery.start();

    // Initially should be in ControlPlane mode
    assert(discovery.get_discovery_mode() == DiscoveryMode::ControlPlane);

    // Switch to gossip-only mode
    discovery.switch_to_gossip_only();
    assert(discovery.get_discovery_mode() == DiscoveryMode::GossipOnly);

    std::cout << "  Discovery mode switching test PASSED" << std::endl;

    discovery.stop();
}

int main() {
    std::cout << "=== Running Gossip Protocol Tests ===" << std::endl;

    try {
        test_gossip_message_types();
        test_node_registration();
        test_chunk_announcement();
        test_peer_discovery();
        test_gossip_message_creation();
        test_gossip_message_handling();
        test_chunk_rarity();
        test_discovery_mode_switching();

        std::cout << "\n=== All Gossip Tests PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
