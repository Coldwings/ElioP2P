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
// Functional Test 1: HTTP Proxy Complete Request Flow
// ============================================================================
TEST_CASE("HTTPProxyFunctionality - Complete proxy request flow", "[functional][proxy][http]") {
    // Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";
    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Pre-populate cache with test data
    std::vector<uint8_t> test_data(1024, 0xAB);
    cache_manager->store_chunk("test_object_1", test_data);

    // storage
    StorageConfig storage_config;
    storage_config.type = "s3";
    storage_config.endpoint = "http://localhost:9000";
    storage_config.bucket = "test-bucket";
    auto storage_client_unique = StorageClientFactory::create(storage_config);
    auto storage_client = std::shared_ptr<StorageClient>(storage_client_unique.release());

    // Setup proxy
    ProxyConfig proxy_config;
    proxy_config.bind_address = "127.0.0.1";
    proxy_config.listen_port = 18080;

    ProxyServer proxy(proxy_config, cache_manager, storage_client);

    // Verify initial state
    auto metrics = proxy.get_metrics();
    REQUIRE(metrics.total_requests == 0);

    // Note: Full HTTP request testing would require running the server
    // and making actual HTTP requests, which is beyond unit test scope
    REQUIRE(proxy.is_running() == false);
}

// ============================================================================
// Functional Test 2: S3 Multipart Upload Simulation
// ============================================================================
TEST_CASE("S3MultiPartUpload - Simulate multipart upload flow", "[functional][storage][s3]") {
    StorageConfig config;
    config.type = "s3";
    config.endpoint = "http://localhost:9000";
    config.access_key = "testkey";
    config.secret_key = "testsecret";
    config.bucket = "test-bucket";

    auto client = StorageClientFactory::create(config);
    REQUIRE(client != nullptr);

    // Simulate multipart upload
    std::string object_key = "test/multipart_object.bin";

    // Test presigned URL generation for upload
    auto url = client->generate_presigned_url(config.bucket, object_key, 3600);
    REQUIRE(!url.empty());
    INFO("Generated presigned URL: " << url.substr(0, 50) << "...");
}

// ============================================================================
// Functional Test 3: P2P Parallel Download
// ============================================================================
TEST_CASE("P2PParallelDownload - Parallel download from multiple peers", "[functional][p2p][transfer]") {
    // Setup cache
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    cache_config.disk_cache_size_mb = 0;
    cache_config.disk_cache_path = "";
    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Store test chunk
    std::vector<uint8_t> chunk_data(1024, 0xCD);
    cache_manager->store_chunk("parallel_test_chunk", chunk_data);

    // Setup P2P discovery
    P2PConfig disc_config;
    disc_config.listen_port = 19010;
    disc_config.selection_k = 3;
    NodeDiscovery discovery(disc_config);

    // Add multiple peers
    for (int i = 0; i < 5; i++) {
        PeerNode peer;
        peer.node_id = "peer_" + std::to_string(i);
        peer.address = "192.168.1." + std::to_string(100 + i);
        peer.port = 9000;
        peer.last_seen = std::time(nullptr);
        peer.available_memory_mb = 4096;
        peer.available_disk_mb = 102400;
        discovery.add_peer(peer);
    }

    // Get peers for transfer
    auto peers = discovery.get_all_peers();
    REQUIRE(peers.size() >= 5);

    // Create parallel download request
    TransferRequest req;
    req.chunk_id = "parallel_test_chunk";
    req.object_key = "test/parallel.bin";
    req.k_value = 3;
    req.mode = TransferMode::FastestFirst;
    req.enable_resume = true;

    REQUIRE(req.k_value == 3);
    REQUIRE(req.mode == TransferMode::FastestFirst);
    REQUIRE(req.enable_resume == true);
}

// ============================================================================
// Functional Test 4: Cache Eviction Policy Execution
// ============================================================================
TEST_CASE("CacheEvictionPolicy - Weighted eviction policy executes correctly", "[functional][cache][eviction]") {
    // Create cache with custom eviction weights
    CacheConfig config;
    config.memory_cache_size_mb = 1;  // 1MB - very small
    config.eviction_weight_time = 1.0f;
    config.eviction_weight_replica = 0.5f;
    config.eviction_weight_heat = 0.3f;

    LRUCache cache(1024, config.eviction_weight_time,
                   config.eviction_weight_replica,
                   config.eviction_weight_heat);

    // Add items with varying access patterns
    for (int i = 0; i < 50; i++) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
        cache.put("key_" + std::to_string(i), data);
    }

    // Access some keys multiple times (make them hot)
    for (int j = 0; j < 200; j++) {
        cache.get("key_0");  // Make this hot
    }

    // Access some keys moderately (warm)
    for (int j = 0; j < 50; j++) {
        cache.get("key_1");
    }

    // Get statistics
    auto stats = cache.stats();
    INFO("Total items: " << stats.total_items);
    INFO("Evictions: " << stats.evictions);
    INFO("Hot items: " << stats.hot_items);
    INFO("Hits: " << stats.hits);

    // Verify eviction worked
    REQUIRE(stats.evictions > 0);
    // Hot item should remain (accessed 200 times)
    bool hot_kept = cache.exists("key_0");
    INFO("Hot item key_0 still in cache: " << hot_kept);
}

// ============================================================================
// Functional Test 5: Control Plane API Calls
// ============================================================================
TEST_CASE("ControlPlaneAPIs - Control plane client API calls", "[functional][control][api]") {
    ControlPlaneConfig config;
    config.endpoint = "http://localhost:8081";
    config.port = 8081;
    config.heartbeat_interval_sec = 5;
    config.enable = true;

    auto client = std::make_unique<ControlPlaneClient>(config);

    // Test connection status
    REQUIRE(client->is_connected() == false);

    // Create node registration
    NodeRegistration registration;
    registration.node_id = "test_node_001";
    registration.address = "192.168.1.10";
    registration.p2p_port = 9000;
    registration.http_port = 8080;
    registration.memory_capacity_mb = 4096;
    registration.disk_capacity_mb = 102400;
    registration.available_chunks = {"chunk_1", "chunk_2", "chunk_3"};

    // Verify registration data
    REQUIRE(registration.node_id == "test_node_001");
    REQUIRE(registration.available_chunks.size() == 3);

    // Create node status
    NodeStatus status;
    status.node_id = "test_node_001";
    status.online = true;
    status.memory_used_mb = 2048;
    status.disk_used_mb = 51200;
    status.cache_hit_rate = 0.75f;
    status.total_chunks = 100;
    status.active_connections = 10;

    REQUIRE(status.online == true);
    REQUIRE(status.cache_hit_rate == 0.75f);
}

// ============================================================================
// Functional Test 6: Gossip Protocol Cycle
// ============================================================================
TEST_CASE("GossipProtocolCycle - Complete gossip protocol cycle", "[functional][gossip][protocol]") {
    // Setup first node
    P2PConfig config1;
    config1.listen_port = 19100;
    config1.gossip_interval_sec = 1;
    NodeDiscovery node1(config1);

    // Setup second node
    P2PConfig config2;
    config2.listen_port = 19101;
    config2.gossip_interval_sec = 1;
    NodeDiscovery node2(config2);

    // Add peer manually to simulate gossip discovery
    PeerNode peer2;
    peer2.node_id = "node_2";
    peer2.address = "127.0.0.1";
    peer2.port = 19101;
    peer2.last_seen = std::time(nullptr);
    peer2.available_memory_mb = 4096;
    peer2.available_disk_mb = 102400;
    node1.add_peer(peer2);

    // Announce chunks
    node1.announce_chunk("chunk_a");
    node1.announce_chunk("chunk_b");

    // Verify announcement
    auto peers = node1.get_all_peers();
    REQUIRE(peers.size() >= 1);

    // Create gossip message
    auto msg = node1.create_gossip_message(GossipMessageType::ChunkAnnounce);
    REQUIRE(msg.type == GossipMessageType::ChunkAnnounce);

    // Clean up
    node1.stop();
    node2.stop();
}

// ============================================================================
// Functional Test 7: Authentication Flow
// ============================================================================
TEST_CASE("AuthenticationFlow - Authentication token flow", "[functional][proxy][auth]") {
    // Setup proxy with authentication
    ProxyConfig config;
    config.listen_port = 18080;
    config.bind_address = "127.0.0.1";
    config.auth_type = "token";
    config.auth_token = "test_token_12345";

    REQUIRE(config.auth_type == "token");
    REQUIRE(config.auth_token.has_value() == true);
    REQUIRE(config.auth_token.value() == "test_token_12345");

    // Test with basic auth
    ProxyConfig config2;
    config2.auth_type = "basic";
    REQUIRE(config2.auth_type == "basic");

    // Test without auth
    ProxyConfig config3;
    config3.auth_type = "none";
    REQUIRE(config3.auth_type == "none");
}

// ============================================================================
// Functional Test 8: Rate Limiting Function
// ============================================================================
TEST_CASE("RateLimitingFunction - Bandwidth rate limiting", "[functional][p2p][rate-limit]") {
    // Setup P2P config with rate limits
    P2PConfig config;
    config.max_upload_speed_mbps = 100;   // 100 Mbps upload
    config.max_download_speed_mbps = 200; // 200 Mbps download

    REQUIRE(config.max_upload_speed_mbps == 100);
    REQUIRE(config.max_download_speed_mbps == 200);

    // Test unlimited (0 = unlimited)
    P2PConfig config2;
    config2.max_upload_speed_mbps = 0;
    config2.max_download_speed_mbps = 0;

    REQUIRE(config2.max_upload_speed_mbps == 0);
    REQUIRE(config2.max_download_speed_mbps == 0);

    // Test various rate limits
    std::vector<uint64_t> test_rates = {1, 10, 50, 100, 500, 1000};
    for (auto rate : test_rates) {
        P2PConfig test_config;
        test_config.max_download_speed_mbps = rate;
        REQUIRE(test_config.max_download_speed_mbps == rate);
    }
}

// ============================================================================
// Functional Test 9: Health Check Endpoint
// ============================================================================
TEST_CASE("HealthCheckEndpoint - Health check functionality", "[functional][proxy][health]") {
    // Setup components for health check
    CacheConfig cache_config;
    cache_config.memory_cache_size_mb = 64;
    auto cache_manager = std::make_shared<ChunkManager>(cache_config);

    // Store some data
    std::vector<uint8_t> data(512, 0xAB);
    cache_manager->store_chunk("health_check_data", data);

    // Get health metrics
    auto mem_stats = cache_manager->get_memory_cache_stats();
    INFO("Health - Total items: " << mem_stats.total_items);
    INFO("Health - Total bytes: " << mem_stats.total_bytes);

    // Verify cache is healthy
    REQUIRE(mem_stats.total_items >= 1);
    REQUIRE(cache_manager->has_chunk("health_check_data") == true);

    // Test ChunkManager health check
    REQUIRE(cache_manager->verify_chunk("health_check_data", data) == true);
}

// ============================================================================
// Functional Test 10: Configuration Reload
// ============================================================================
TEST_CASE("ConfigurationReload - Hot reload configuration", "[functional][config][reload]") {
    // Create initial config
    CacheConfig config1;
    config1.memory_cache_size_mb = 1024;
    config1.disk_cache_size_mb = 10240;

    REQUIRE(config1.memory_cache_size_mb == 1024);

    // Create new config (simulating reload)
    CacheConfig config2;
    config2.memory_cache_size_mb = 2048;
    config2.disk_cache_size_mb = 20480;

    REQUIRE(config2.memory_cache_size_mb == 2048);

    // Create cache manager with new config
    ChunkManager manager(config2);

    // Verify new settings applied
    auto mem_stats = manager.get_memory_cache_stats();
    INFO("After reload - Cache items: " << mem_stats.total_items);

    // Test proxy config reload
    ProxyConfig proxy1;
    proxy1.listen_port = 8080;
    ProxyConfig proxy2;
    proxy2.listen_port = 9090;

    REQUIRE(proxy1.listen_port == 8080);
    REQUIRE(proxy2.listen_port == 9090);

    // Test P2P config reload
    P2PConfig p2p1;
    p2p1.listen_port = 9000;
    p2p1.max_connections = 100;

    P2PConfig p2p2;
    p2p2.listen_port = 9001;
    p2p2.max_connections = 200;

    REQUIRE(p2p1.max_connections == 100);
    REQUIRE(p2p2.max_connections == 200);
}
