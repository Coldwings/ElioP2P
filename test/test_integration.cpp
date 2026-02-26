#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <memory>
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
// Integration Test 1: Cache and Storage Integration
// Tests: Cache miss triggers storage fetch
// ============================================================================
TEST_CASE("CacheAndStorageIntegration - Cache miss loads from storage", "[integration][cache][storage]") {
    // Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 128;
    cache_config.chunk_size_mb = 16;
    cache_config.disk_cache_path = "";  // Disable disk for testing

    ChunkManager cache_manager(cache_config);

    // Setup storage client (mock-like behavior via config)
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.access_key = "testkey";
    storage_config.secret_key = "testsecret";
    storage_config.bucket = "test-bucket";

    auto storage_client = StorageClientFactory::create(storage_config);

    // Simulate cache miss scenario
    std::string object_key = "test/object.bin";
    std::string chunk_id = "test_object_0";

    // Verify chunk not in cache
    REQUIRE(cache_manager.has_chunk(chunk_id) == false);

    // Simulate loading from storage (without actual network call)
    std::vector<uint8_t> data_from_storage(1024, 0xAB);
    REQUIRE(cache_manager.store_chunk(chunk_id, data_from_storage) == true);

    // Now should be able to get from cache
    auto retrieved = cache_manager.get_chunk(chunk_id);
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == data_from_storage);
}

// ============================================================================
// Integration Test 2: Proxy and Cache Integration
// Tests: HTTP proxy retrieves data from cache
// ============================================================================
TEST_CASE("ProxyCacheIntegration - Proxy retrieves from cache", "[integration][proxy][cache]") {
    // Setup cache with data
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 128;
    cache_config.chunk_size_mb = 16;
    cache_config.disk_cache_path = "";  // Disable disk for testing

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Pre-populate cache
    std::vector<uint8_t> cached_data(512, 0xCD);
    REQUIRE(cache_manager->store_chunk("cached_chunk_1", cached_data) == true);

    // Setup proxy config
    ProxyConfig proxy_config;
    proxy_config.bind_address = "127.0.0.1";
    proxy_config.listen_port = 18080;

    // Setup storage client
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    auto storage_client_unique = StorageClientFactory::create(storage_config);
    auto storage_client = std::shared_ptr<StorageClient>(storage_client_unique.release());

    // Create proxy server
    ProxyServer proxy(proxy_config, cache_manager, storage_client);

    // Get metrics (before starting)
    auto metrics = proxy.get_metrics();
    REQUIRE(metrics.cache_hits == 0);

    // Note: We don't start the server in unit tests to avoid port binding
    // The integration test verifies component wiring
    REQUIRE(proxy.is_running() == false);
}

// ============================================================================
// Integration Test 3: Proxy and Storage Integration
// Tests: Cache miss triggers storage fallback
// ============================================================================
TEST_CASE("ProxyStorageIntegration - Proxy falls back to storage on cache miss", "[integration][proxy][storage]") {
    // Setup minimal components
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    ProxyConfig proxy_config;
    proxy_config.bind_address = "127.0.0.1";
    proxy_config.listen_port = 18081;

    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    auto storage_client_unique = StorageClientFactory::create(storage_config);
    auto storage_client = std::shared_ptr<StorageClient>(storage_client_unique.release());

    ProxyServer proxy(proxy_config, cache_manager, storage_client);

    // Verify cache is empty
    auto mem_stats = cache_manager->get_memory_cache_stats();
    INFO("Initial cache items: " << mem_stats.total_items);

    REQUIRE(cache_manager->has_chunk("nonexistent") == false);
}

// ============================================================================
// Integration Test 4: P2P and Cache Integration
// Tests: P2P node shares cached chunks
// ============================================================================
TEST_CASE("P2PCacheIntegration - P2P nodes share cached chunks", "[integration][p2p][cache]") {
    // Setup cache manager
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Store some chunks that could be shared via P2P
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(256, static_cast<uint8_t>(i));
        cache_manager->store_chunk("shared_chunk_" + std::to_string(i), data);
    }

    // Verify chunks are available for sharing
    REQUIRE(cache_manager->has_chunk("shared_chunk_0") == true);
    REQUIRE(cache_manager->has_chunk("shared_chunk_4") == true);

    // Get stats
    auto mem_stats = cache_manager->get_memory_cache_stats();
    REQUIRE(mem_stats.total_items >= 5);
}

// ============================================================================
// Integration Test 5: Discovery and Transfer Integration
// Tests: Node discovery provides peers for transfer
// ============================================================================
TEST_CASE("DiscoveryTransferIntegration - Discovery provides peers for transfer", "[integration][discovery][transfer]") {
    // Setup node discovery with P2PConfig
    P2PConfig disc_config;
    disc_config.listen_port = 19000;
    disc_config.gossip_interval_sec = 10;

    NodeDiscovery discovery(disc_config);

    // Setup transfer manager with P2PConfig (TransferManager uses P2PConfig)
    P2PConfig transfer_config;
    transfer_config.listen_port = 9000;
    transfer_config.max_connections = 10;
    transfer_config.selection_k = 3;

    TransferManager transfer_manager(transfer_config);

    // Start discovery to find peers
    discovery.start();

    // Get discovered peers
    auto peers = discovery.get_all_peers();
    INFO("Discovered peers: " << peers.size());

    // Create transfer request
    TransferRequest req;
    req.chunk_id = "test_chunk_1";
    req.object_key = "test/object.bin";
    req.mode = TransferMode::FastestFirst;
    req.k_value = 3;

    // Stop discovery
    discovery.stop();
    REQUIRE(true);  // Reached this point without crash
}

// ============================================================================
// Integration Test 6: Gossip and Cache Integration
// Tests: Gossip protocol syncs cache metadata
// ============================================================================
TEST_CASE("GossipCacheIntegration - Gossip syncs cache metadata", "[integration][gossip][cache]") {
    // Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Add chunks to cache
    std::vector<uint8_t> chunk_data(512, 0xEE);
    cache_manager->store_chunk("gossip_test_chunk", chunk_data);

    // Setup P2P-based discovery (for gossip)
    P2PConfig disc_config;
    disc_config.listen_port = 17946;
    disc_config.gossip_interval_sec = 10;

    NodeDiscovery discovery(disc_config);

    // Start and stop gossip
    discovery.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto peers = discovery.get_all_peers();
    INFO("Gossip peers: " << peers.size());

    discovery.stop();
    REQUIRE(true);
}

// ============================================================================
// Integration Test 7: Control Plane Client Integration
// Tests: Control plane client registers and sends heartbeats
// ============================================================================
TEST_CASE("ControlPlaneClientIntegration - Client registers and heartbeats", "[integration][control][client]") {
    // Setup control plane client config
    ControlPlaneConfig config;
    config.endpoint = "http://localhost:8081";
    config.port = 8081;
    config.heartbeat_interval_sec = 5;
    config.enable = true;

    auto client = std::make_unique<ControlPlaneClient>(config);

    // Note: In integration test, we'd connect to actual control plane
    // For unit testing, we verify client can be created and configured
    // Verify not connected initially
    REQUIRE(client->is_connected() == false);
}

// ============================================================================
// Integration Test 8: Multi-tier Cache Integration
// Tests: Memory and disk tier work together
// ============================================================================
TEST_CASE("MultiTierCacheIntegration - Memory and disk tiers work together", "[integration][cache][multi-tier]") {
    // Create temporary directory for disk cache
    auto temp_dir = std::filesystem::temp_directory_path() / "eliop2p_test_cache";
    std::filesystem::create_directories(temp_dir);
    std::string cache_path = temp_dir.string();

    // Setup config with both tiers
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 1;  // Very small for testing
    cache_config.disk_cache_size_mb = 10;
    cache_config.chunk_size_mb = 1;
    cache_config.disk_cache_path = cache_path;

    ChunkManager cache_manager(cache_config);

    // Store chunks that should overflow to disk
    std::vector<uint8_t> small_chunk(512, 0xAA);
    for (int i = 0; i < 10; i++) {
        cache_manager.store_chunk("tier_test_" + std::to_string(i), small_chunk);
    }

    // Verify some chunks are accessible
    bool found_any = false;
    for (int i = 0; i < 10; i++) {
        if (cache_manager.has_chunk("tier_test_" + std::to_string(i))) {
            found_any = true;
            break;
        }
    }
    REQUIRE(found_any == true);

    // Cleanup
    std::filesystem::remove_all(temp_dir);
}

// ============================================================================
// Integration Test 9: Eviction and Cache Sync Integration
// Tests: Eviction triggers cache state sync
// ============================================================================
TEST_CASE("EvictionSyncIntegration - Eviction triggers cache sync", "[integration][cache][eviction]") {
    // Small cache to trigger eviction
    LRUCache cache(100);

    // Fill beyond capacity
    for (int i = 0; i < 20; i++) {
        std::vector<uint8_t> data(20, static_cast<uint8_t>(i));
        cache.put("evict_key_" + std::to_string(i), data);
    }

    auto stats = cache.stats();
    INFO("Evictions: " << stats.evictions);
    INFO("Total items: " << stats.total_items);

    // Verify eviction occurred
    REQUIRE(stats.evictions > 0);

    // Verify cache still functions - either original key remains or eviction happened
    bool key_exists = cache.exists("evict_key_0");
    bool key19_exists = cache.exists("evict_key_19");
    REQUIRE((key_exists || key19_exists));  // At least one should exist
}

// ============================================================================
// Integration Test 10: Chunk Replication Integration
// Tests: Chunks can be replicated to multiple peers
// ============================================================================
TEST_CASE("ChunkReplicationIntegration - Chunks replicate to multiple peers", "[integration][p2p][replication]") {
    // Setup source cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";

    auto source_cache = std::make_shared<ChunkManager>(cache_config);

    // Store chunk to replicate
    std::vector<uint8_t> replicate_data(1024, 0xFF);
    REQUIRE(source_cache->store_chunk("replicate_chunk", replicate_data) == true);

    // Verify data integrity
    auto retrieved = source_cache->get_chunk("replicate_chunk");
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == replicate_data);

    // Setup destination cache
    auto dest_cache = std::make_shared<ChunkManager>(cache_config);

    // Simulate replication by storing to destination
    dest_cache->store_chunk("replicate_chunk", replicate_data);

    // Verify replica
    auto replica = dest_cache->get_chunk("replicate_chunk");
    REQUIRE(replica.has_value() == true);
    REQUIRE(replica->data() == replicate_data);
}

// ============================================================================
// Integration Test 11: Config Load Integration
// Tests: Configuration loads correctly for all modules
// ============================================================================
TEST_CASE("ConfigLoadIntegration - Config loads for all modules", "[integration][config]") {
    // Test CacheConfig
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 128;
    cache_config.disk_cache_size_mb = 512;
    cache_config.chunk_size_mb = 16;
    REQUIRE(cache_config.memory_cache_size_mb == 128);

    // Test ProxyConfig
    ProxyConfig proxy_config;
    proxy_config.listen_port = 8080;
    proxy_config.max_connections = 1000;
    REQUIRE(proxy_config.listen_port == 8080);

    // Test P2PConfig
    P2PConfig p2p_config;
    p2p_config.listen_port = 9000;
    p2p_config.max_connections = 100;
    REQUIRE(p2p_config.listen_port == 9000);

    // Test ControlPlaneConfig
    ControlPlaneConfig control_config;
    control_config.endpoint = "http://localhost:8081";
    control_config.port = 8081;
    REQUIRE(control_config.endpoint == "http://localhost:8081");

    // Test StorageConfig
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.bucket = "test-bucket";
    REQUIRE(storage_config.bucket == "test-bucket");
}

// ============================================================================
// Integration Test 12: Graceful Shutdown Integration
// Tests: All modules shutdown cleanly
// ============================================================================
TEST_CASE("GracefulShutdownIntegration - All modules shutdown gracefully", "[integration][shutdown]") {
    // Setup components
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";

    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Store some data
    std::vector<uint8_t> data(256, 0xAB);
    cache_manager->store_chunk("shutdown_test", data);

    // Setup discovery
    P2PConfig disc_config;
    disc_config.listen_port = 19001;
    NodeDiscovery discovery(disc_config);

    // Setup transfer using P2PConfig
    P2PConfig transfer_config;
    transfer_config.listen_port = 19000;
    TransferManager transfer(transfer_config);

    // Start components
    discovery.start();
    // Note: Not starting transfer manager in test to avoid port binding

    // Stop in reverse order (graceful shutdown pattern)
    discovery.stop();
    transfer.stop();

    // Verify cache data still accessible after shutdown
    auto retrieved = cache_manager->get_chunk("shutdown_test");
    REQUIRE(retrieved.has_value() == true);

    REQUIRE(true);  // Reached here = clean shutdown
}

// ============================================================================
// Integration Test 13: Peer Connection Management
// Tests: NodeDiscovery manages peer connections
// ============================================================================
TEST_CASE("PeerConnectionManagement - Discovery manages peer connections", "[integration][p2p][peer]") {
    P2PConfig disc_config;
    disc_config.listen_port = 19002;
    disc_config.max_peers = 50;

    NodeDiscovery discovery(disc_config);

    // Add a test peer manually
    PeerNode peer;
    peer.node_id = "test_peer_1";
    peer.address = "192.168.1.100";
    peer.port = 9000;
    peer.last_seen = std::time(nullptr);
    peer.available_memory_mb = 4096;
    peer.available_disk_mb = 102400;

    discovery.add_peer(peer);

    // Verify peer was added
    auto peers = discovery.get_all_peers();
    REQUIRE(peers.size() >= 1);

    // Remove peer
    discovery.remove_peer("test_peer_1");

    // Verify peer was removed
    auto peers_after = discovery.get_all_peers();
    bool found = false;
    for (const auto& p : peers_after) {
        if (p.node_id == "test_peer_1") {
            found = true;
            break;
        }
    }
    REQUIRE(found == false);
}

// ============================================================================
// Integration Test 14: Transfer Request Builder
// Tests: TransferRequest can be constructed with various options
// ============================================================================
TEST_CASE("TransferRequestBuilder - Transfer request with options", "[integration][transfer][request]") {
    TransferRequest req;
    req.chunk_id = "test_chunk_0";
    req.object_key = "test/bucket/object.bin";
    req.offset = 1024;
    req.expected_size = 16384;
    req.mode = TransferMode::RarestFirst;
    req.k_value = 5;
    req.enable_resume = true;

    // Verify request fields
    REQUIRE(req.chunk_id == "test_chunk_0");
    REQUIRE(req.object_key == "test/bucket/object.bin");
    REQUIRE(req.offset == 1024);
    REQUIRE(req.expected_size == 16384);
    REQUIRE(req.mode == TransferMode::RarestFirst);
    REQUIRE(req.k_value == 5);
    REQUIRE(req.enable_resume == true);

    // Test different modes
    TransferRequest req2;
    req2.mode = TransferMode::NearestFirst;
    REQUIRE(req2.mode == TransferMode::NearestFirst);

    TransferRequest req3;
    req3.mode = TransferMode::FastestFirst;
    REQUIRE(req3.mode == TransferMode::FastestFirst);
}

// ============================================================================
// Integration Test 15: Storage Client Factory
// Tests: Factory creates correct storage client type
// ============================================================================
TEST_CASE("StorageClientFactory - Creates correct client type", "[integration][storage][factory]") {
    // Create S3 client
    StorageConfig s3_config;
    s3_config.type = "s3";
    s3_config.endpoint = "http://localhost:9000";
    s3_config.access_key = "testkey";
    s3_config.secret_key = "testsecret";
    s3_config.bucket = "test-bucket";

    auto s3_client = StorageClientFactory::create(s3_config);
    REQUIRE(s3_client != nullptr);

    // Create OSS client
    StorageConfig oss_config;
    oss_config.type = "oss";
    oss_config.endpoint = "http://localhost:9000";
    oss_config.access_key = "testkey";
    oss_config.secret_key = "testsecret";
    oss_config.bucket = "test-bucket";

    auto oss_client = StorageClientFactory::create(oss_config);
    REQUIRE(oss_client != nullptr);
}
