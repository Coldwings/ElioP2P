#ifndef ELIOP2P_CACHE_CHUNK_MANAGER_H
#define ELIOP2P_CACHE_CHUNK_MANAGER_H

#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/base/config.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <unordered_map>
#include <filesystem>

namespace eliop2p {

// Chunk metadata stored alongside the chunk
struct ChunkMetadata {
    ChunkMetadata() = default;

    std::string chunk_id;
    std::string object_key;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string hash;  // SHA256
    uint64_t version = 0;
    bool is_immutable = true;
    uint32_t replica_count = 1;
    HeatLevel heat_level = HeatLevel::Cold;
    uint64_t last_access_time = 0;
    uint64_t creation_time = 0;
};

class ChunkManager {
public:
    ChunkManager(const CacheConfig& config);
    ~ChunkManager();

    // Get chunk data - checks memory cache first, then disk
    std::optional<Chunk> get_chunk(const std::string& chunk_id);

    // Store chunk data
    bool store_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data);

    // Store chunk with metadata
    bool store_chunk(const ChunkMetadata& metadata, const std::vector<uint8_t>& data);

    // Remove chunk
    bool remove_chunk(const std::string& chunk_id);

    // Check if chunk exists
    bool has_chunk(const std::string& chunk_id) const;

    // Get chunk metadata
    std::optional<ChunkMetadata> get_metadata(const std::string& chunk_id) const;

    // Update chunk metadata
    void update_metadata(const ChunkMetadata& metadata);

    // Compute chunk ID from object key and offset
    static std::string compute_chunk_id(const std::string& object_key, uint64_t offset, uint64_t chunk_size);

    // Get cache statistics
    CacheStats get_memory_cache_stats() const;
    CacheStats get_disk_cache_stats() const;

    // Set disk cache path
    void set_disk_cache_path(const std::string& path);

    // Persist chunk to disk
    bool persist_to_disk(const std::string& chunk_id, const std::vector<uint8_t>& data);

    // Load chunk from disk
    std::optional<std::vector<uint8_t>> load_from_disk(const std::string& chunk_id) const;

    // Verify chunk data with SHA256
    bool verify_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data) const;

    // Compute SHA256 hash of data
    static std::string compute_sha256(const std::vector<uint8_t>& data);

    // Get total cache size
    uint64_t total_memory_usage() const;
    uint64_t total_disk_usage() const;

    // Check if should evict from memory to disk
    bool should_promote_to_disk() const;
    bool should_demote_from_memory() const;

    // Manual promotion/demotion
    bool promote_to_memory(const std::string& chunk_id);
    bool demote_to_disk(const std::string& chunk_id);

    // Trigger eviction on both caches
    void trigger_eviction();

    // Initialize disk cache directory
    bool initialize_disk_cache();

    // Sync metadata to disk
    bool sync_metadata() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_CACHE_CHUNK_MANAGER_H
