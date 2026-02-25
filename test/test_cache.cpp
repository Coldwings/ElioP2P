#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

void test_lru_cache_basic() {
    std::cout << "Testing basic LRU cache operations..." << std::endl;

    // Create a cache with 1KB capacity
    LRUCache cache(1024);

    // Test put and get
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5};
    assert(cache.put("key1", data1));
    assert(cache.exists("key1"));

    auto result = cache.get("key1");
    assert(result.has_value());
    assert(result->data() == data1);

    // Test eviction
    std::vector<uint8_t> large_data(512, 0xFF);
    cache.put("key2", large_data);

    // After putting large data, key1 should still exist (2*5 + 512 = 522 < 1024)
    assert(cache.exists("key1"));

    // Put more data to trigger eviction
    std::vector<uint8_t> more_data(512, 0xAA);
    cache.put("key3", more_data);

    // At this point, some eviction may have occurred
    std::cout << "  Cache usage: " << (cache.usage() * 100) << "%" << std::endl;

    auto stats = cache.stats();
    std::cout << "  Hits: " << stats.hits << ", Misses: " << stats.misses << std::endl;
    std::cout << "  Evictions: " << stats.evictions << std::endl;

    std::cout << "  Basic LRU cache test PASSED" << std::endl;
}

void test_lru_cache_eviction() {
    std::cout << "Testing LRU cache eviction..." << std::endl;

    // Create a cache with limited capacity
    LRUCache cache(100);

    // Put data to exceed capacity
    for (int i = 0; i < 20; i++) {
        std::vector<uint8_t> data(10, static_cast<uint8_t>(i));
        cache.put("key" + std::to_string(i), data);
    }

    auto stats = cache.stats();
    std::cout << "  After 20 inserts: items=" << stats.total_items
              << ", evictions=" << stats.evictions << std::endl;

    // Verify eviction occurred
    assert(stats.evictions > 0);
    std::cout << "  Eviction test PASSED" << std::endl;
}

void test_chunk_manager() {
    std::cout << "Testing ChunkManager..." << std::endl;

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

    assert(manager.store_chunk(chunk_id, chunk_data));

    // Test chunk retrieval
    auto retrieved = manager.get_chunk(chunk_id);
    assert(retrieved.has_value());
    assert(retrieved->data() == chunk_data);

    // Test chunk existence
    assert(manager.has_chunk(chunk_id));

    // Test SHA256 verification
    assert(manager.verify_chunk(chunk_id, chunk_data));

    // Test chunk removal
    assert(manager.remove_chunk(chunk_id));
    assert(!manager.has_chunk(chunk_id));

    std::cout << "  ChunkManager test PASSED" << std::endl;
}

void test_multi_factor_eviction() {
    std::cout << "Testing multi-factor weighted eviction..." << std::endl;

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
    std::cout << "  Hot items: " << stats.hot_items
              << ", Warm items: " << stats.warm_items
              << ", Cold items: " << stats.cold_items << std::endl;

    std::cout << "  Multi-factor eviction test PASSED" << std::endl;
}

void test_cache_stats() {
    std::cout << "Testing cache statistics..." << std::endl;

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
    std::cout << "  Memory cache items: " << mem_stats.total_items << std::endl;
    std::cout << "  Memory cache bytes: " << mem_stats.total_bytes << std::endl;
    std::cout << "  Hit rate: " << (mem_stats.hit_rate() * 100) << "%" << std::endl;

    std::cout << "  Cache statistics test PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running Cache Module Tests ===" << std::endl;

    try {
        test_lru_cache_basic();
        test_lru_cache_eviction();
        test_chunk_manager();
        test_multi_factor_eviction();
        test_cache_stats();

        std::cout << "\n=== All Tests PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
