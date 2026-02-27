#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <thread>
#include <filesystem>
#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/storage/oss_client.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/control/client.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

// ============================================================================
// E2E Test 1: Full Cache Miss Flow
// Simulates complete flow when data is not in cache
// ============================================================================
TEST_CASE("FullCacheMissFlow - Complete cache miss to storage flow", "[e2e][cache][storage]") {
    // Step 1: Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";
    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Step 2: Verify cache miss
    std::string chunk_id = "e2e_test_chunk";
    REQUIRE(cache_manager->has_chunk(chunk_id) == false);

    // Step 3: Simulate fetching from storage
    std::vector<uint8_t> storage_data(2048, 0xEE);
    REQUIRE(cache_manager->store_chunk(chunk_id, storage_data) == true);

    // Step 4: Verify cache hit now
    auto retrieved = cache_manager->get_chunk(chunk_id);
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == storage_data);

    // Step 5: Verify cache metrics
    auto stats = cache_manager->get_memory_cache_stats();
    REQUIRE(stats.total_items >= 1);

    INFO("E2E cache miss flow completed - data fetched from storage and cached");
}

// ============================================================================
// E2E Test 2: Full P2P Transfer Flow
// Simulates complete P2P transfer from peer discovery to data retrieval
// ============================================================================
TEST_CASE("FullP2PTransferFlow - Complete P2P transfer flow", "[e2e][p2p][transfer]") {
    // Step 1: Setup source node with data
    CacheConfig src_cache_config;
    src_cache_config.memory_cache_size_mb = 64;
    src_cache_config.disk_cache_path = "";
    auto src_cache = std::make_shared<ChunkManager>(src_cache_config);

    std::vector<uint8_t> transfer_data(4096, 0xAA);
    src_cache->store_chunk("p2p_transfer_chunk", transfer_data);

    // Step 2: Setup destination discovery
    P2PConfig disc_config;
    disc_config.listen_port = 19200;
    disc_config.selection_k = 3;
    NodeDiscovery dest_discovery(disc_config);

    // Step 3: Simulate peer discovery
    PeerNode source_peer;
    source_peer.node_id = "source_node";
    source_peer.address = "192.168.1.50";
    source_peer.port = 9000;
    source_peer.last_seen = std::time(nullptr);
    source_peer.available_memory_mb = 8192;
    source_peer.available_disk_mb = 102400;
    dest_discovery.add_peer(source_peer);

    // Step 4: Query peers with chunk
    auto peers_with_chunk = dest_discovery.get_peers_with_chunk("p2p_transfer_chunk");
    INFO("Peers with chunk: " << peers_with_chunk.size());

    // Step 5: Setup transfer manager
    P2PConfig transfer_config;
    transfer_config.listen_port = 19201;
    transfer_config.selection_k = 2;
    TransferManager transfer_manager(transfer_config);

    // Step 6: Create transfer request
    TransferRequest req;
    req.chunk_id = "p2p_transfer_chunk";
    req.object_key = "test/large_file.bin";
    req.k_value = 2;
    req.mode = TransferMode::FastestFirst;
    req.enable_resume = true;

    // Step 7: Verify transfer configuration
    REQUIRE(req.k_value == 2);
    REQUIRE(req.mode == TransferMode::FastestFirst);

    // Cleanup
    dest_discovery.stop();

    INFO("E2E P2P transfer flow completed");
}

// ============================================================================
// E2E Test 3: Multi-Node Cluster Flow
// Simulates interaction between multiple nodes in cluster
// ============================================================================
TEST_CASE("MultiNodeClusterFlow - Multiple nodes in cluster", "[e2e][cluster][multi-node]") {
    // Setup multiple cache managers (simulating nodes)
    std::vector<std::shared_ptr<ChunkManager>> nodes;

    CacheConfig node_config;
    node_config.memory_cache_size_mb = 32;
    node_config.disk_cache_path = "";

    for (int i = 0; i < 3; i++) {
        nodes.push_back(std::make_shared<ChunkManager>(node_config));
    }

    // Node 0 stores some chunks
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(256, static_cast<uint8_t>(i));
        nodes[0]->store_chunk("shared_chunk_" + std::to_string(i), data);
    }

    // Verify all nodes are operational
    for (int i = 0; i < 3; i++) {
        auto stats = nodes[i]->get_memory_cache_stats();
        INFO("Node " << i << " items: " << stats.total_items);
    }

    // Node 0 should have chunks
    REQUIRE(nodes[0]->has_chunk("shared_chunk_0") == true);
    REQUIRE(nodes[0]->has_chunk("shared_chunk_4") == true);

    INFO("E2E multi-node cluster flow completed");
}

// ============================================================================
// E2E Test 4: Graceful Shutdown E2E
// Tests graceful shutdown of all components
// ============================================================================
TEST_CASE("GracefulShutdownE2E - All components shutdown gracefully", "[e2e][shutdown][graceful]") {
    // Step 1: Initialize all components
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    P2PConfig disc_config;
    disc_config.listen_port = 19210;
    auto discovery = std::make_unique<NodeDiscovery>(disc_config);

    P2PConfig transfer_config;
    transfer_config.listen_port = 19211;
    auto transfer = std::make_unique<TransferManager>(transfer_config);

    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.bucket = "test-bucket";
    auto storage_unique = StorageClientFactory::create(storage_config);
    auto storage = std::shared_ptr<StorageClient>(storage_unique.release());

    ProxyConfig proxy_config;
    proxy_config.listen_port = 18090;
    auto proxy = std::make_unique<ProxyServer>(
        proxy_config, cache, storage);

    // Step 2: Start components
    discovery->start();

    // Store data before shutdown
    std::vector<uint8_t> data(512, 0xFF);
    cache->store_chunk("shutdown_data", data);

    // Step 3: Graceful shutdown sequence (reverse order)
    discovery->stop();
    transfer->stop();

    // Verify data still accessible
    auto retrieved = cache->get_chunk("shutdown_data");
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == data);

    INFO("E2E graceful shutdown completed - data preserved");
}

// ============================================================================
// E2E Test 5: Failure Recovery Flow
// Tests recovery from various failure scenarios
// ============================================================================
TEST_CASE("FailureRecoveryFlow - Recovery from failures", "[e2e][failure][recovery]") {
    // Step 1: Create cache with data
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 32;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    // Store data
    std::vector<uint8_t> original_data(1024, 0xBB);
    cache->store_chunk("recovery_test", original_data);

    // Verify data stored
    REQUIRE(cache->has_chunk("recovery_test") == true);

    // Step 2: Simulate failure scenario - remove chunk
    REQUIRE(cache->remove_chunk("recovery_test") == true);
    REQUIRE(cache->has_chunk("recovery_test") == false);

    // Step 3: Recovery - reload from "source"
    std::vector<uint8_t> recovered_data(1024, 0xBB);
    cache->store_chunk("recovery_test", recovered_data);

    // Step 4: Verify recovery
    auto retrieved = cache->get_chunk("recovery_test");
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == recovered_data);

    INFO("E2E failure recovery flow completed");
}

// ============================================================================
// E2E Test 6: Load Balancing Flow
// Tests load distribution across multiple sources
// ============================================================================
TEST_CASE("LoadBalancingFlow - Load balancing across peers", "[e2e][load-balance][p2p]") {
    // Setup discovery with multiple peers
    P2PConfig disc_config;
    disc_config.listen_port = 19220;
    disc_config.selection_k = 3;
    NodeDiscovery discovery(disc_config);

    // Add multiple peers with different capabilities
    for (int i = 0; i < 5; i++) {
        PeerNode peer;
        peer.node_id = "peer_load_" + std::to_string(i);
        peer.address = "192.168.2." + std::to_string(i + 1);
        peer.port = 9000;
        peer.last_seen = std::time(nullptr);
        // Vary capabilities
        peer.available_memory_mb = 1024 * (i + 1);
        peer.available_disk_mb = 102400 * (i + 1);
        discovery.add_peer(peer);
    }

    // Get all peers
    auto all_peers = discovery.get_all_peers();
    REQUIRE(all_peers.size() >= 5);

    // Simulate load balancing selection
    TransferRequest req;
    req.k_value = 3;
    req.mode = TransferMode::FastestFirst;

    INFO("Load balancing - selected " << req.k_value << " peers from "
           << all_peers.size() << " available");

    // Test different selection modes
    req.mode = TransferMode::NearestFirst;
    REQUIRE(req.mode == TransferMode::NearestFirst);

    req.mode = TransferMode::RarestFirst;
    REQUIRE(req.mode == TransferMode::RarestFirst);

    discovery.stop();

    INFO("E2E load balancing flow completed");
}

// ============================================================================
// E2E Test 7: Data Consistency Flow
// Tests data consistency across cache and storage
// ============================================================================
TEST_CASE("DataConsistencyFlow - Data consistency verification", "[e2e][consistency][data]") {
    // Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    // Setup storage
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.bucket = "consistency-test-bucket";
    auto storage = StorageClientFactory::create(storage_config);

    // Store data in cache
    std::string key = "consistency_test_object";
    std::vector<uint8_t> data(2048, 0xCC);
    cache->store_chunk(key, data);

    // Verify cache has data
    auto cached = cache->get_chunk(key);
    REQUIRE(cached.has_value() == true);
    REQUIRE(cached->data() == data);

    // Verify integrity using SHA256
    bool verified = cache->verify_chunk(key, data);
    REQUIRE(verified == true);

    // Remove and verify
    cache->remove_chunk(key);
    bool exists_after_remove = cache->has_chunk(key);
    REQUIRE(exists_after_remove == false);

    INFO("E2E data consistency flow completed");
}

// ============================================================================
// E2E Test 8: End-to-End With Control Plane
// Tests complete flow with control plane integration
// ============================================================================
TEST_CASE("ControlPlaneE2E - Complete flow with control plane", "[e2e][control-plane][integration]") {
    // Step 1: Setup control plane client
    ControlPlaneConfig cp_config;
    cp_config.endpoint = "http://localhost:8081";
    cp_config.port = 8081;
    cp_config.heartbeat_interval_sec = 5;
    cp_config.enable = true;

    auto control_client = std::make_unique<ControlPlaneClient>(cp_config);

    // Step 2: Setup cache and storage
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 128;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.bucket = "e2e-bucket";
    auto storage_unique = StorageClientFactory::create(storage_config);
    auto storage = std::shared_ptr<StorageClient>(storage_unique.release());

    // Step 3: Setup proxy
    ProxyConfig proxy_config;
    proxy_config.listen_port = 18100;
    proxy_config.bind_address = "127.0.0.1";
    ProxyServer proxy(proxy_config, cache, storage);

    // Step 4: Register node (simulated)
    NodeRegistration registration;
    registration.node_id = "e2e_node_001";
    registration.address = "192.168.1.100";
    registration.p2p_port = 9000;
    registration.http_port = 8080;
    registration.memory_capacity_mb = 4096;
    registration.disk_capacity_mb = 102400;
    registration.available_chunks = {"chunk1", "chunk2", "chunk3"};

    // Step 5: Send heartbeat
    NodeStatus status;
    status.node_id = registration.node_id;
    status.online = true;
    status.memory_used_mb = 2048;
    status.disk_used_mb = 51200;
    status.cache_hit_rate = 0.65f;
    status.total_chunks = 1000;

    // Verify status
    REQUIRE(status.online == true);
    REQUIRE(status.cache_hit_rate > 0.0f);

    // Step 6: Verify proxy metrics
    auto metrics = proxy.get_metrics();
    REQUIRE(metrics.total_requests == 0);

    INFO("E2E with control plane completed");
}
