#include "eliop2p/cache/lru_cache.h"
#include "eliop2p/base/logger.h"
#include <chrono>
#include <algorithm>
#include <list>
#include <unordered_map>
#include <cmath>

namespace eliop2p {

Chunk::Chunk(const std::string& key, const std::vector<uint8_t>& data)
    : key_(key), data_(data) {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    last_access_time_ = now;
    creation_time_ = now;
}

void Chunk::set_last_access_time(uint64_t time) {
    last_access_time_ = time;
}

void Chunk::compute_heat_level(uint64_t current_time, uint32_t hot_threshold, uint32_t warm_threshold) const {
    // Calculate time since last access in hours
    double hours_since_access = static_cast<double>(current_time - last_access_time_) / 3600.0;
    if (hours_since_access <= 0) hours_since_access = 0.01; // Avoid division by zero

    // Calculate accesses per hour
    double accesses_per_hour = static_cast<double>(access_count_) / hours_since_access;

    if (accesses_per_hour > hot_threshold) {
        heat_level_ = HeatLevel::Hot;
    } else if (accesses_per_hour >= warm_threshold) {
        heat_level_ = HeatLevel::Warm;
    } else {
        heat_level_ = HeatLevel::Cold;
    }
}

float Chunk::compute_eviction_score(uint64_t current_time,
                                     float w1, float w2, float w3) const {
    // Eviction score = w1 * time since last access + w2 * replica count + w3 * heat level
    // Higher score = more likely to be evicted

    double time_since_access = static_cast<double>(current_time - last_access_time_);

    // Convert heat level to numeric value (higher = more likely to evict)
    double heat_value = static_cast<double>(heat_level_);

    float score = w1 * static_cast<float>(time_since_access) +
                  w2 * static_cast<float>(replica_count_) +
                  w3 * heat_value;

    return score;
}

struct LRUCache::Impl {
    uint64_t max_capacity_;
    uint64_t current_size_ = 0;
    CacheStats stats_;

    // Thread safety: recursive_mutex to allow nested locking
    // (e.g., get() calling touch() while holding the lock)
    mutable std::recursive_mutex mutex_;

    // Eviction weights for multi-factor LRU
    float eviction_weight_time_ = 1.0f;
    float eviction_weight_replica_ = 0.5f;
    float eviction_weight_heat_ = 0.3f;

    // Heat thresholds
    uint32_t hot_threshold_ = 100;
    uint32_t warm_threshold_ = 10;

    // Protection period in seconds (prevent eviction right after access)
    uint64_t eviction_protection_sec_ = 60;

    // LRU list: most recently used at front
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, Chunk> cache_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_map_;

    EvictionCallback eviction_callback;

    Impl(uint64_t max_capacity)
        : max_capacity_(max_capacity) {}

    Impl(uint64_t max_capacity, float w1, float w2, float w3)
        : max_capacity_(max_capacity),
          eviction_weight_time_(w1),
          eviction_weight_replica_(w2),
          eviction_weight_heat_(w3) {}

    uint64_t current_time() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void touch(const std::string& key) {
        auto it = lru_map_.find(key);
        if (it != lru_map_.end()) {
            lru_list_.erase(it->second);
            lru_list_.push_front(key);
            it->second = lru_list_.begin();
        }
    }

    // Multi-factor weighted eviction: selects item with highest eviction score
    std::string select_eviction_target() {
        if (lru_list_.empty()) return "";

        uint64_t now = current_time();
        std::string target;
        float max_score = -1.0f;

        // Find item with highest eviction score
        for (const auto& key : lru_list_) {
            auto chunk_it = cache_.find(key);
            if (chunk_it == cache_.end()) continue;

            // Skip items in protection period
            if (now - chunk_it->second.last_access_time() < eviction_protection_sec_) {
                continue;
            }

            float score = chunk_it->second.compute_eviction_score(
                now,
                eviction_weight_time_,
                eviction_weight_replica_,
                eviction_weight_heat_);

            if (score > max_score) {
                max_score = score;
                target = key;
            }
        }

        // If all items are protected, select the oldest one anyway
        if (target.empty()) {
            target = lru_list_.back();
        }

        return target;
    }

    void evict_one() {
        if (lru_list_.empty()) return;

        std::string key = select_eviction_target();
        if (key.empty()) return;

        auto lru_it = lru_map_.find(key);
        if (lru_it != lru_map_.end()) {
            lru_list_.erase(lru_it->second);
            lru_map_.erase(lru_it);
        }

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            current_size_ -= it->second.size();
            if (eviction_callback) {
                eviction_callback(key, it->second.data());
            }
            cache_.erase(it);
            stats_.evictions++;
        }
    }

    void update_heat_counts() {
        uint64_t now = current_time();
        stats_.hot_items = 0;
        stats_.warm_items = 0;
        stats_.cold_items = 0;

        for (const auto& pair : cache_) {
            // Create a non-const copy to compute heat level since it's mutable
            Chunk& chunk = const_cast<Chunk&>(pair.second);
            chunk.compute_heat_level(now, hot_threshold_, warm_threshold_);
            switch (chunk.heat_level()) {
                case HeatLevel::Hot: stats_.hot_items++; break;
                case HeatLevel::Warm: stats_.warm_items++; break;
                case HeatLevel::Cold: stats_.cold_items++; break;
            }
        }
    }
};

LRUCache::LRUCache(uint64_t max_capacity_bytes)
    : impl_(std::make_unique<Impl>(max_capacity_bytes)) {}

LRUCache::LRUCache(uint64_t max_capacity_bytes,
                    float eviction_weight_time,
                    float eviction_weight_replica,
                    float eviction_weight_heat)
    : impl_(std::make_unique<Impl>(max_capacity_bytes,
                                   eviction_weight_time,
                                   eviction_weight_replica,
                                   eviction_weight_heat)) {}

LRUCache::~LRUCache() = default;

LRUCache::LRUCache(LRUCache&& other) noexcept = default;
LRUCache& LRUCache::operator=(LRUCache&& other) noexcept = default;

std::optional<Chunk> LRUCache::get(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);

    auto it = impl_->cache_.find(key);
    if (it == impl_->cache_.end()) {
        impl_->stats_.misses++;
        return std::nullopt;
    }

    impl_->touch(key);
    impl_->stats_.hits++;

    // Update access time and count
    uint64_t now = impl_->current_time();
    it->second.set_last_access_time(now);
    it->second.increment_access_count();

    // Recompute heat level
    it->second.compute_heat_level(now, impl_->hot_threshold_, impl_->warm_threshold_);

    return it->second;
}

bool LRUCache::put(const std::string& key, const std::vector<uint8_t>& data) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);

    // Check if key already exists
    auto it = impl_->cache_.find(key);
    if (it != impl_->cache_.end()) {
        // Update existing
        impl_->current_size_ -= it->second.size();
        it->second = Chunk(key, data);
        impl_->current_size_ += data.size();
        impl_->touch(key);
        return true;
    }

    // Evict if necessary
    while (impl_->current_size_ + data.size() > impl_->max_capacity_ &&
           !impl_->lru_list_.empty()) {
        impl_->evict_one();
    }

    // Insert new
    impl_->cache_[key] = Chunk(key, data);
    impl_->lru_list_.push_front(key);
    impl_->lru_map_[key] = impl_->lru_list_.begin();
    impl_->current_size_ += data.size();
    impl_->stats_.total_items++;
    impl_->stats_.total_bytes += data.size();

    // Initial heat level
    uint64_t now = impl_->current_time();
    impl_->cache_[key].compute_heat_level(now, impl_->hot_threshold_, impl_->warm_threshold_);

    // Trigger eviction check after putting new data
    maybe_evict();

    return true;
}

bool LRUCache::remove(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);

    auto it = impl_->cache_.find(key);
    if (it == impl_->cache_.end()) {
        return false;
    }

    impl_->current_size_ -= it->second.size();
    impl_->stats_.total_items--;
    impl_->stats_.total_bytes -= it->second.size();

    auto lru_it = impl_->lru_map_.find(key);
    if (lru_it != impl_->lru_map_.end()) {
        impl_->lru_list_.erase(lru_it->second);
        impl_->lru_map_.erase(lru_it);
    }

    impl_->cache_.erase(it);
    return true;
}

bool LRUCache::exists(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->cache_.find(key) != impl_->cache_.end();
}

void LRUCache::clear() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->cache_.clear();
    impl_->lru_list_.clear();
    impl_->lru_map_.clear();
    impl_->current_size_ = 0;
    impl_->stats_ = CacheStats();
}

void LRUCache::set_eviction_callback(EvictionCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->eviction_callback = std::move(callback);
}

CacheStats LRUCache::stats() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->update_heat_counts();
    return impl_->stats_;
}

void LRUCache::maybe_evict() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    float usage = static_cast<float>(impl_->current_size_) / impl_->max_capacity_;
    if (usage > 0.8f) {
        // Evict until usage is below 60%
        while (usage > 0.6f && !impl_->lru_list_.empty()) {
            impl_->evict_one();
            usage = static_cast<float>(impl_->current_size_) / impl_->max_capacity_;
        }
    }
}

void LRUCache::evict_to_target() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    float usage = static_cast<float>(impl_->current_size_) / impl_->max_capacity_;
    while (usage > 0.6f && !impl_->lru_list_.empty()) {
        impl_->evict_one();
        usage = static_cast<float>(impl_->current_size_) / impl_->max_capacity_;
    }
}

float LRUCache::usage() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return static_cast<float>(impl_->current_size_) / impl_->max_capacity_;
}

uint64_t LRUCache::current_size() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->current_size_;
}

uint64_t LRUCache::max_capacity() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->max_capacity_;
}

void LRUCache::update_chunk_metadata(const std::string& key,
                                      uint32_t replica_count,
                                      HeatLevel heat_level) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    auto it = impl_->cache_.find(key);
    if (it != impl_->cache_.end()) {
        it->second.set_replica_count(replica_count);
        it->second.set_heat_level(heat_level);
    }
}

void LRUCache::set_heat_thresholds(uint32_t hot_threshold, uint32_t warm_threshold) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->hot_threshold_ = hot_threshold;
    impl_->warm_threshold_ = warm_threshold;
}

} // namespace eliop2p
