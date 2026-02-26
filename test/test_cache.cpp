#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

TEST_CASE("LRU Cache Basic Operations", "[cache][lru]") {
    // Create a cache with 1KB capacity
    LRUCache cache(1024);

    // Test put and get
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5};
    REQUIRE(cache.put("key1", data1) == true);
    REQUIRE(cache.exists("key1") == true);

    auto result = cache.get("key1");
    REQUIRE(result.has_value() == true);
    REQUIRE(result->data() == data1);

    // Test eviction
    std::vector<uint8_t> large_data(512, 0xFF);
    cache.put("key2", large_data);

    // After putting large data, key1 should still exist (2*5 + 512 = 522 < 1024)
    REQUIRE(cache.exists("key1") == true);

    // Put more data to trigger eviction
    std::vector<uint8_t> more_data(512, 0xAA);
    cache.put("key3", more_data);

    // At this point, some eviction may have occurred
    INFO("Cache usage: " << (cache.usage() * 100) << "%");

    auto stats = cache.stats();
    INFO("Hits: " << stats.hits << ", Misses: " << stats.misses);
    INFO("Evictions: " << stats.evictions);
}

TEST_CASE("LRU Cache Eviction", "[cache][lru][eviction]") {
    // Create a cache with limited capacity
    LRUCache cache(100);

    // Put data to exceed capacity
    for (int i = 0; i < 20; i++) {
        std::vector<uint8_t> data(10, static_cast<uint8_t>(i));
        cache.put("key" + std::to_string(i), data);
    }

    auto stats = cache.stats();
    INFO("After 20 inserts: items=" << stats.total_items
              << ", evictions=" << stats.evictions);

    // Verify eviction occurred
    REQUIRE(stats.evictions > 0);
}

TEST_CASE("ChunkManager Basic Operations", "[cache][chunk]") {
    CacheConfig config;
    config.memory_cache_size_mb = 64;  // 64MB
    config.disk_cache_size_mb = 128;   // 128MB
    config.chunk_size_mb = 16;
    config.eviction_threshold = 0.8;
    config.eviction_target = 0.6;
    config.eviction_weight_time = 1.0f;
    config.eviction_weight_replica = 0.5f;
    config.eviction_weight_heat = 0.3f;
    config.hot_threshold = 100;
    config.warm_threshold = 10;
    config.disk_cache_path = "";  // Disable disk cache for testing

    ChunkManager manager(config);

    // Test chunk storage
    std::vector<uint8_t> chunk_data(1024, 0x42);
    std::string chunk_id = "test_object_0";

    REQUIRE(manager.store_chunk(chunk_id, chunk_data) == true);

    // Test chunk retrieval
    auto retrieved = manager.get_chunk(chunk_id);
    REQUIRE(retrieved.has_value() == true);
    REQUIRE(retrieved->data() == chunk_data);

    // Test chunk existence
    REQUIRE(manager.has_chunk(chunk_id) == true);

    // Test SHA256 verification
    REQUIRE(manager.verify_chunk(chunk_id, chunk_data) == true);

    // Test chunk removal
    REQUIRE(manager.remove_chunk(chunk_id) == true);
    REQUIRE(manager.has_chunk(chunk_id) == false);
}

TEST_CASE("Multi-Factor Weighted Eviction", "[cache][eviction][multi-factor]") {
    // Create cache with custom eviction weights
    LRUCache cache(100, 1.0f, 0.5f, 0.3f);

    // Add some chunks with different metadata
    for (int i = 0; i < 5; i++) {
        std::vector<uint8_t> data(20, static_cast<uint8_t>(i));
        cache.put("key" + std::to_string(i), data);
    }

    // Access some keys multiple times to change heat level
    for (int j = 0; j < 150; j++) {
        cache.get("key0");  // Make key0 hot
    }

    auto stats = cache.stats();
    INFO("Hot items: " << stats.hot_items);
    INFO("Warm items: " << stats.warm_items);
    INFO("Cold items: " << stats.cold_items);
}

TEST_CASE("Cache Statistics", "[cache][stats]") {
    CacheConfig config;
    config.memory_cache_size_mb = 64;
    config.hot_threshold = 100;
    config.warm_threshold = 10;
    config.disk_cache_path = "";  // Disable disk cache

    ChunkManager manager(config);

    // Add some chunks
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
        manager.store_chunk("chunk_" + std::to_string(i), data);
    }

    // Access some chunks
    for (int j = 0; j < 5; j++) {
        manager.get_chunk("chunk_0");
        manager.get_chunk("chunk_1");
    }

    auto mem_stats = manager.get_memory_cache_stats();
    INFO("Memory cache items: " << mem_stats.total_items);
    INFO("Memory cache bytes: " << mem_stats.total_bytes);
    INFO("Hit rate: " << (mem_stats.hit_rate() * 100) << "%");
}
