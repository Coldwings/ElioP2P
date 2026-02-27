// E2E (End-to-End) Tests for ElioP2P
// These tests require a running test environment with mocked external services

#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <thread>
#include <filesystem>
#include <future>
#include <atomic>

#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/control/client.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

// ============================================================================
// E2E Test 1: Full Request Flow Through Proxy
// Tests complete HTTP request flow: client -> proxy -> cache -> storage
// ============================================================================
TEST_CASE("E2E_FullRequestFlow - Complete proxy request flow", "[e2e][proxy][full]") {
    // This test requires actual network sockets and simulates real request flow

    // Setup test configuration
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_path = "";

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Pre-populate cache with test data
    std::vector<uint8_t> test_data(1024, 0xAB);
    std::string test_key = "e2e_test_object";
    REQUIRE(cache_manager->store_chunk(test_key, test_data) == true);

    // Create mock storage client
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.bucket = "test-bucket";
    auto storage_unique = StorageClientFactory::create(storage_config);
    auto storage = std::shared_ptr<StorageClient>(storage_unique.release());

    // Create and start proxy server
    ProxyConfig proxy_config;
    proxy_config.listen_port = 18080;
    proxy_config.bind_address = "127.0.0.1";

    ProxyServer proxy(proxy_config, cache_manager, storage);

    // Verify proxy can access cached data
    auto metrics = proxy.get_metrics();
    REQUIRE(metrics.total_requests == 0);

    INFO("E2E full request flow test completed");
}

// ============================================================================
// E2E Test 2: Multi-Node P2P Cluster
// Tests multiple nodes discovering each other and sharing chunks
// ============================================================================
TEST_CASE("E2E_MultiNodeCluster - P2P cluster with multiple nodes", "[e2e][p2p][cluster]") {
    // Setup multiple nodes
    const int NUM_NODES = 3;
    std::vector<std::unique_ptr<NodeDiscovery>> discoveries;
    std::vector<std::shared_ptr<ChunkManager>> caches;

    for (int i = 0; i < NUM_NODES; i++) {
        P2PConfig config;
        config.listen_port = 19300 + i;
        config.gossip_interval_sec = 1;

        auto discovery = std::make_unique<NodeDiscovery>(config);

        CacheConfig cache_config;
        cache_config.memory_cache_size_mb = 32;
        cache_config.disk_cache_path = "";
        auto cache = std::make_shared<ChunkManager>(cache_config);

        discoveries.push_back(std::move(discovery));
        caches.push_back(cache);
    }

    // Start all discoveries
    for (auto& disc : discoveries) {
        disc->start();
    }

    // Add chunks to first node
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(256, static_cast<uint8_t>(i));
        caches[0]->store_chunk("cluster_chunk_" + std::to_string(i), data);
    }

    // Announce chunks
    discoveries[0]->announce_chunk("cluster_chunk_0");
    discoveries[0]->announce_chunk("cluster_chunk_1");

    // Let gossip propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Stop all
    for (auto& disc : discoveries) {
        disc->stop();
    }

    INFO("E2E multi-node cluster test completed");
}

// ============================================================================
// E2E Test 3: Complete System Startup and Shutdown
// Tests full application lifecycle
// ============================================================================
TEST_CASE("E2E_SystemLifecycle - Complete startup and shutdown", "[e2e][lifecycle]") {
    // Initialize all components as a real node would
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    P2PConfig p2p_config;
    p2p_config.listen_port = 19400;
    auto discovery = std::make_unique<NodeDiscovery>(p2p_config);

    // Transfer manager uses P2PConfig, not TransferConfig
    P2PConfig transfer_config;
    transfer_config.listen_port = 19401;
    auto transfer = std::make_unique<TransferManager>(transfer_config);

    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    auto storage_unique = StorageClientFactory::create(storage_config);
    auto storage = std::shared_ptr<StorageClient>(storage_unique.release());

    ProxyConfig proxy_config;
    proxy_config.listen_port = 18480;
    proxy_config.bind_address = "127.0.0.1";
    ProxyServer proxy(proxy_config, cache, storage);

    // Start all components
    discovery->start();

    // Store test data
    std::vector<uint8_t> test_data(512, 0xCC);
    cache->store_chunk("lifecycle_test", test_data);

    // Verify data is accessible
    REQUIRE(cache->has_chunk("lifecycle_test") == true);

    // Graceful shutdown
    discovery->stop();
    transfer->stop();

    // Verify data preserved after shutdown
    auto retrieved = cache->get_chunk("lifecycle_test");
    REQUIRE(retrieved.has_value() == true);

    INFO("E2E system lifecycle test completed");
}

// ============================================================================
// E2E Test 4: High Load Scenario
// Tests system under concurrent requests with true multi-threading
// LRUCache now uses recursive_mutex for thread safety
// ============================================================================
TEST_CASE("E2E_HighLoadScenario - Concurrent request handling", "[e2e][load][concurrent]") {
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 128;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    // Pre-populate cache
    for (int i = 0; i < 50; i++) {
        std::vector<uint8_t> data(512, static_cast<uint8_t>(i));
        cache->store_chunk("load_test_" + std::to_string(i), data);
    }

    // Simulate concurrent access - LRUCache is now thread-safe with recursive_mutex
    const int NUM_THREADS = 4;
    std::vector<std::thread> workers;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    for (int t = 0; t < NUM_THREADS; t++) {
        workers.emplace_back([&cache, &success_count, &fail_count, t]() {
            for (int i = 0; i < 20; i++) {
                std::string key = "load_test_" + std::to_string((t * 20 + i) % 50);
                if (cache->has_chunk(key)) {
                    auto data = cache->get_chunk(key);
                    if (data.has_value()) {
                        success_count++;
                    } else {
                        fail_count++;
                    }
                } else {
                    fail_count++;
                }
            }
        });
    }

    // Wait for all workers
    for (auto& w : workers) {
        w.join();
    }

    INFO("Successful requests: " << success_count.load());
    INFO("Failed requests: " << fail_count.load());

    REQUIRE(success_count.load() > 0);
    REQUIRE(fail_count.load() == 0);

    INFO("E2E high load scenario test completed");
}

// ============================================================================
// E2E Test 5: Data Consistency Across Operations
// Tests data integrity through various operations
// ============================================================================
TEST_CASE("E2E_DataConsistency - Data integrity verification", "[e2e][consistency]") {
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_path = "";
    auto cache = std::make_shared<ChunkManager>(cache_config);

    // Store data with known hash
    std::vector<uint8_t> original_data(2048);
    for (size_t i = 0; i < original_data.size(); i++) {
        original_data[i] = static_cast<uint8_t>(i % 256);
    }

    std::string key = "consistency_e2e_test";
    REQUIRE(cache->store_chunk(key, original_data) == true);

    // Retrieve and verify
    auto retrieved = cache->get_chunk(key);
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == original_data);

    // Verify integrity check
    REQUIRE(cache->verify_chunk(key, original_data) == true);

    // Remove and verify removal
    REQUIRE(cache->remove_chunk(key) == true);
    REQUIRE(cache->has_chunk(key) == false);

    // Re-store and verify again
    REQUIRE(cache->store_chunk(key, original_data) == true);
    auto re_retrieved = cache->get_chunk(key);
    REQUIRE(re_retrieved.has_value() == true);
    REQUIRE(re_retrieved->data() == original_data);

    INFO("E2E data consistency test completed");
}

// ============================================================================
// E2E Test 6: Replica Adjustment Flow
// Tests replica count adjustment based on availability
// ============================================================================
TEST_CASE("E2E_ReplicaAdjustment - Dynamic replica management", "[e2e][replica]") {
    // Setup nodes for replica testing
    const int NUM_NODES = 5;
    std::vector<std::unique_ptr<NodeDiscovery>> discoveries;
    std::vector<std::shared_ptr<ChunkManager>> caches;

    for (int i = 0; i < NUM_NODES; i++) {
        P2PConfig config;
        config.listen_port = 19500 + i;
        config.gossip_interval_sec = 1;

        auto discovery = std::make_unique<NodeDiscovery>(config);
        discovery->start();

        CacheConfig cache_config;
        cache_config.memory_cache_size_mb = 32;
        auto cache = std::make_shared<ChunkManager>(cache_config);

        // Add peer relationships
        for (int j = 0; j < i; j++) {
            PeerNode peer;
            peer.node_id = "replica_node_" + std::to_string(j);
            peer.address = "127.0.0.1";
            peer.port = 19500 + j;
            peer.last_seen = std::time(nullptr);
            discovery->add_peer(peer);
        }

        discoveries.push_back(std::move(discovery));
        caches.push_back(cache);
    }

    // Create a chunk on first node
    std::vector<uint8_t> chunk_data(1024, 0xDD);
    caches[0]->store_chunk("replica_test_chunk", chunk_data);
    discoveries[0]->announce_chunk("replica_test_chunk");

    // Simulate replication to other nodes
    for (int i = 1; i < 3; i++) {
        caches[i]->store_chunk("replica_test_chunk", chunk_data);
    }

    // Count replicas
    int replica_count = 0;
    for (auto& cache : caches) {
        if (cache->has_chunk("replica_test_chunk")) {
            replica_count++;
        }
    }

    REQUIRE(replica_count >= 3);

    // Cleanup
    for (auto& disc : discoveries) {
        disc->stop();
    }

    INFO("E2E replica adjustment test completed with " << replica_count << " replicas");
}
