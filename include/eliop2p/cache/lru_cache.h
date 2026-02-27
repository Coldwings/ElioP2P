#ifndef ELIOP2P_CACHE_LRU_CACHE_H
#define ELIOP2P_CACHE_LRU_CACHE_H

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <mutex>

namespace eliop2p {

// Heat level enum for cache tiers
enum class HeatLevel {
    Cold = 0,  // <10 accesses/hour
    Warm = 1,  // 10-100 accesses/hour
    Hot = 2    // >100 accesses/hour
};

class Chunk {
public:
    Chunk() = default;
    Chunk(const std::string& key, const std::vector<uint8_t>& data);

    const std::string& key() const { return key_; }
    const std::vector<uint8_t>& data() const { return data_; }
    std::vector<uint8_t>& data() { return data_; }
    size_t size() const { return data_.size(); }

    void set_last_access_time(uint64_t time);
    uint64_t last_access_time() const { return last_access_time_; }

    void set_access_count(uint64_t count) { access_count_ = count; }
    uint64_t access_count() const { return access_count_; }
    void increment_access_count() { access_count_++; }

    void set_replica_count(uint32_t count) { replica_count_ = count; }
    uint32_t replica_count() const { return replica_count_; }

    void set_heat_level(HeatLevel level) { heat_level_ = level; }
    HeatLevel heat_level() const { return heat_level_; }

    // Mutable for const compute_heat_level
    mutable HeatLevel heat_level_ = HeatLevel::Cold;

    // Compute heat level based on access count per hour
    void compute_heat_level(uint64_t current_time, uint32_t hot_threshold, uint32_t warm_threshold) const;

    // Compute eviction score: w1*time + w2*replica + w3*heat
    float compute_eviction_score(uint64_t current_time,
                                 float w1, float w2, float w3) const;

    void set_creation_time(uint64_t time) { creation_time_ = time; }
    uint64_t creation_time() const { return creation_time_; }

    // For SSD cache: track if data is persisted
    void set_persisted(bool persisted) { persisted_ = persisted; }
    bool persisted() const { return persisted_; }

private:
    std::string key_;
    std::vector<uint8_t> data_;
    uint64_t last_access_time_ = 0;
    uint64_t creation_time_ = 0;
    uint64_t access_count_ = 0;
    uint32_t replica_count_ = 1;
    bool persisted_ = false;
};

struct CacheStats {
    uint64_t total_items = 0;
    uint64_t total_bytes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    uint64_t hot_items = 0;
    uint64_t warm_items = 0;
    uint64_t cold_items = 0;
    float hit_rate() const {
        uint64_t total = hits + misses;
        return total > 0 ? static_cast<float>(hits) / total : 0.0f;
    }
};

class LRUCache {
public:
    using EvictionCallback = std::function<void(const std::string& key, const std::vector<uint8_t>& data)>;

    LRUCache(uint64_t max_capacity_bytes);
    LRUCache(uint64_t max_capacity_bytes,
             float eviction_weight_time,
             float eviction_weight_replica,
             float eviction_weight_heat);
    ~LRUCache();

    // Disable copy
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // Enable move
    LRUCache(LRUCache&& other) noexcept;
    LRUCache& operator=(LRUCache&& other) noexcept;

    std::optional<Chunk> get(const std::string& key);
    bool put(const std::string& key, const std::vector<uint8_t>& data);
    bool remove(const std::string& key);
    bool exists(const std::string& key) const;

    void clear();

    void set_eviction_callback(EvictionCallback callback);

    CacheStats stats() const;

    // Trigger eviction if needed
    void maybe_evict();

    // Force eviction to target level
    void evict_to_target();

    // Get current capacity usage
    float usage() const;
    uint64_t current_size() const;
    uint64_t max_capacity() const;

    // Update chunk metadata for eviction scoring
    void update_chunk_metadata(const std::string& key,
                               uint32_t replica_count,
                               HeatLevel heat_level);

    // Set heat thresholds
    void set_heat_thresholds(uint32_t hot_threshold, uint32_t warm_threshold);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_CACHE_LRU_CACHE_H
