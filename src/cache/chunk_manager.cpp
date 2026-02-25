#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/base/logger.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Include Elio hash for SHA256
#include <elio/hash/sha256.hpp>

namespace eliop2p {

namespace fs = std::filesystem;

struct ChunkManager::Impl {
    CacheConfig config;
    std::unique_ptr<LRUCache> memory_cache;
    std::unique_ptr<LRUCache> disk_cache;
    std::unordered_map<std::string, ChunkMetadata> metadata;
    std::string disk_cache_path;
    bool initialized = false;

    Impl(const CacheConfig& cfg)
        : config(cfg),
          memory_cache(std::make_unique<LRUCache>(
              cfg.memory_cache_size_mb * 1024 * 1024,
              cfg.eviction_weight_time,
              cfg.eviction_weight_replica,
              cfg.eviction_weight_heat)),
          disk_cache(std::make_unique<LRUCache>(
              cfg.disk_cache_size_mb * 1024 * 1024,
              cfg.eviction_weight_time,
              cfg.eviction_weight_replica,
              cfg.eviction_weight_heat)) {

        // Set heat thresholds
        memory_cache->set_heat_thresholds(cfg.hot_threshold, cfg.warm_threshold);
        disk_cache->set_heat_thresholds(cfg.hot_threshold, cfg.warm_threshold);

        // Set eviction callback to persist data to disk when evicted from memory
        memory_cache->set_eviction_callback([this](const std::string& key, const std::vector<uint8_t>& data) {
            // When evicted from memory, try to persist to disk if disk cache is enabled
            if (!disk_cache_path.empty()) {
                persist_to_disk_internal(key, data);
            }
        });

        disk_cache_path = cfg.disk_cache_path;
    }

    bool persist_to_disk_internal(const std::string& chunk_id, const std::vector<uint8_t>& data) {
        if (disk_cache_path.empty()) return false;

        try {
            fs::path chunk_path = fs::path(disk_cache_path) / "chunks" / (chunk_id + ".chunk");
            fs::create_directories(chunk_path.parent_path());

            std::ofstream file(chunk_path, std::ios::binary);
            if (!file) {
                Logger::instance().error("Failed to open chunk file for writing: " + chunk_path.string());
                return false;
            }
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            file.close();

            return true;
        } catch (const std::exception& e) {
            Logger::instance().error("Failed to persist chunk to disk: " + std::string(e.what()));
            return false;
        }
    }

    std::optional<std::vector<uint8_t>> load_from_disk_internal(const std::string& chunk_id) const {
        if (disk_cache_path.empty()) return std::nullopt;

        try {
            fs::path chunk_path = fs::path(disk_cache_path) / "chunks" / (chunk_id + ".chunk");
            if (!fs::exists(chunk_path)) {
                return std::nullopt;
            }

            std::ifstream file(chunk_path, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::nullopt;
            }

            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> data(size);
            if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
                return std::nullopt;
            }

            return data;
        } catch (const std::exception& e) {
            Logger::instance().error("Failed to load chunk from disk: " + std::string(e.what()));
            return std::nullopt;
        }
    }

    bool delete_from_disk(const std::string& chunk_id) const {
        if (disk_cache_path.empty()) return false;

        try {
            fs::path chunk_path = fs::path(disk_cache_path) / "chunks" / (chunk_id + ".chunk");
            if (fs::exists(chunk_path)) {
                fs::remove(chunk_path);
            }
            return true;
        } catch (const std::exception& e) {
            Logger::instance().error("Failed to delete chunk from disk: " + std::string(e.what()));
            return false;
        }
    }

    bool initialize_disk_cache() {
        if (disk_cache_path.empty()) return false;

        try {
            // Create cache directory structure
            fs::path chunks_dir = fs::path(disk_cache_path) / "chunks";
            fs::path metadata_dir = fs::path(disk_cache_path) / "metadata";

            fs::create_directories(chunks_dir);
            fs::create_directories(metadata_dir);

            // Scan existing chunks and rebuild disk cache
            if (fs::exists(chunks_dir)) {
                for (const auto& entry : fs::directory_iterator(chunks_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".chunk") {
                        std::string chunk_id = entry.path().stem().string();
                        auto data = load_from_disk_internal(chunk_id);
                        if (data) {
                            disk_cache->put(chunk_id, *data);
                        }
                    }
                }
            }

            initialized = true;
            Logger::instance().info("Disk cache initialized at: " + disk_cache_path);
            return true;
        } catch (const std::exception& e) {
            Logger::instance().error("Failed to initialize disk cache: " + std::string(e.what()));
            return false;
        }
    }

    uint64_t calculate_disk_usage() const {
        if (disk_cache_path.empty()) return 0;

        try {
            uint64_t total = 0;
            fs::path chunks_dir = fs::path(disk_cache_path) / "chunks";
            if (fs::exists(chunks_dir)) {
                for (const auto& entry : fs::recursive_directory_iterator(chunks_dir)) {
                    if (entry.is_regular_file()) {
                        total += entry.file_size();
                    }
                }
            }
            return total;
        } catch (const std::exception&) {
            return 0;
        }
    }
};

ChunkManager::ChunkManager(const CacheConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ChunkManager::~ChunkManager() = default;

std::optional<Chunk> ChunkManager::get_chunk(const std::string& chunk_id) {
    // Try memory cache first
    auto chunk = impl_->memory_cache->get(chunk_id);
    if (chunk) {
        Logger::instance().debug("Chunk found in memory cache: " + chunk_id);
        return chunk;
    }

    // Try disk cache
    chunk = impl_->disk_cache->get(chunk_id);
    if (chunk) {
        Logger::instance().debug("Chunk found in disk cache: " + chunk_id);
        // Promote to memory cache
        impl_->memory_cache->put(chunk_id, chunk->data());
        return chunk;
    }

    Logger::instance().debug("Chunk not found in any cache: " + chunk_id);
    return std::nullopt;
}

bool ChunkManager::store_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data) {
    // Compute hash for verification
    std::string hash = compute_sha256(data);

    // Create metadata
    ChunkMetadata metadata;
    metadata.chunk_id = chunk_id;
    metadata.hash = hash;
    metadata.size = data.size();
    metadata.is_immutable = true;
    metadata.replica_count = 1;
    metadata.heat_level = HeatLevel::Cold;
    metadata.creation_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    metadata.last_access_time = metadata.creation_time;

    return store_chunk(metadata, data);
}

bool ChunkManager::store_chunk(const ChunkMetadata& metadata, const std::vector<uint8_t>& data) {
    // Verify data size matches expected
    if (metadata.size > 0 && data.size() != metadata.size) {
        Logger::instance().error("Chunk size mismatch for: " + metadata.chunk_id);
        return false;
    }

    // Store in memory cache
    bool success = impl_->memory_cache->put(metadata.chunk_id, data);

    // Update metadata
    impl_->metadata[metadata.chunk_id] = metadata;

    // Persist to disk if path is set
    if (!impl_->disk_cache_path.empty()) {
        persist_to_disk(metadata.chunk_id, data);
        impl_->disk_cache->put(metadata.chunk_id, data);
    }

    Logger::instance().debug("Chunk stored: " + metadata.chunk_id + ", size: " + std::to_string(data.size()));
    return success;
}

bool ChunkManager::remove_chunk(const std::string& chunk_id) {
    bool from_memory = impl_->memory_cache->remove(chunk_id);
    bool from_disk = impl_->disk_cache->remove(chunk_id);

    // Remove from disk
    impl_->delete_from_disk(chunk_id);

    impl_->metadata.erase(chunk_id);
    return from_memory || from_disk;
}

bool ChunkManager::has_chunk(const std::string& chunk_id) const {
    return impl_->memory_cache->exists(chunk_id) ||
           impl_->disk_cache->exists(chunk_id);
}

std::optional<ChunkMetadata> ChunkManager::get_metadata(const std::string& chunk_id) const {
    auto it = impl_->metadata.find(chunk_id);
    if (it != impl_->metadata.end()) {
        return it->second;
    }
    return std::nullopt;
}

void ChunkManager::update_metadata(const ChunkMetadata& metadata) {
    impl_->metadata[metadata.chunk_id] = metadata;

    // Update cache metadata for eviction scoring
    impl_->memory_cache->update_chunk_metadata(
        metadata.chunk_id,
        metadata.replica_count,
        metadata.heat_level);
}

std::string ChunkManager::compute_chunk_id(const std::string& object_key,
                                            uint64_t offset,
                                            uint64_t chunk_size) {
    std::ostringstream oss;
    oss << object_key << "_" << (offset / chunk_size);
    return oss.str();
}

CacheStats ChunkManager::get_memory_cache_stats() const {
    return impl_->memory_cache->stats();
}

CacheStats ChunkManager::get_disk_cache_stats() const {
    CacheStats stats = impl_->disk_cache->stats();
    stats.total_bytes = impl_->calculate_disk_usage();
    return stats;
}

void ChunkManager::set_disk_cache_path(const std::string& path) {
    impl_->disk_cache_path = path;
}

bool ChunkManager::persist_to_disk(const std::string& chunk_id, const std::vector<uint8_t>& data) {
    return impl_->persist_to_disk_internal(chunk_id, data);
}

std::optional<std::vector<uint8_t>> ChunkManager::load_from_disk(const std::string& chunk_id) const {
    return impl_->load_from_disk_internal(chunk_id);
}

bool ChunkManager::verify_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data) const {
    auto it = impl_->metadata.find(chunk_id);
    if (it == impl_->metadata.end()) {
        // No metadata, can't verify
        return true;
    }

    std::string computed_hash = compute_sha256(data);
    return computed_hash == it->second.hash;
}

std::string ChunkManager::compute_sha256(const std::vector<uint8_t>& data) {
    auto digest = elio::hash::sha256(data.data(), data.size());
    return elio::hash::sha256_hex(digest);
}

uint64_t ChunkManager::total_memory_usage() const {
    return impl_->memory_cache->current_size();
}

uint64_t ChunkManager::total_disk_usage() const {
    return impl_->calculate_disk_usage();
}

bool ChunkManager::should_promote_to_disk() const {
    return impl_->disk_cache->usage() < 0.8f;
}

bool ChunkManager::should_demote_from_memory() const {
    return impl_->memory_cache->usage() > 0.8f;
}

bool ChunkManager::promote_to_memory(const std::string& chunk_id) {
    // Check if exists in disk cache
    auto data = impl_->load_from_disk_internal(chunk_id);
    if (!data) {
        return false;
    }

    // Put in memory cache
    return impl_->memory_cache->put(chunk_id, *data);
}

bool ChunkManager::demote_to_disk(const std::string& chunk_id) {
    // Get from memory cache
    auto chunk = impl_->memory_cache->get(chunk_id);
    if (!chunk) {
        return false;
    }

    // Persist to disk
    bool success = persist_to_disk(chunk_id, chunk->data());

    // Remove from memory cache
    if (success) {
        impl_->memory_cache->remove(chunk_id);
    }

    return success;
}

void ChunkManager::trigger_eviction() {
    impl_->memory_cache->maybe_evict();
    impl_->disk_cache->maybe_evict();
}

bool ChunkManager::initialize_disk_cache() {
    return impl_->initialize_disk_cache();
}

bool ChunkManager::sync_metadata() const {
    if (impl_->disk_cache_path.empty()) return false;

    try {
        fs::path metadata_path = fs::path(impl_->disk_cache_path) / "metadata" / "cache_metadata.json";

        // For now, we skip metadata persistence as it would require JSON serialization
        // In production, implement proper metadata persistence
        Logger::instance().debug("Metadata sync not fully implemented");
        return true;
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to sync metadata: " + std::string(e.what()));
        return false;
    }
}

} // namespace eliop2p
