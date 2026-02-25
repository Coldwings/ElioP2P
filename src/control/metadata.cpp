#include "eliop2p/control/metadata.h"
#include "eliop2p/base/logger.h"
#include <fstream>
#include <ctime>

namespace eliop2p {

struct MetadataManager::Impl {
    std::unordered_map<std::string, ChunkIndexEntry> chunks;
    std::unordered_set<std::string> peers;
};

MetadataManager::MetadataManager()
    : impl_(std::make_unique<Impl>()) {}

MetadataManager::~MetadataManager() = default;

void MetadataManager::update_chunk_location(const std::string& chunk_id,
                                              const std::string& peer_id) {
    impl_->chunks[chunk_id].peer_ids.insert(peer_id);
    impl_->chunks[chunk_id].last_update_time = std::time(nullptr);
    impl_->peers.insert(peer_id);
}

void MetadataManager::remove_chunk_location(const std::string& chunk_id,
                                             const std::string& peer_id) {
    auto it = impl_->chunks.find(chunk_id);
    if (it != impl_->chunks.end()) {
        it->second.peer_ids.erase(peer_id);
    }
}

std::optional<std::vector<std::string>> MetadataManager::get_chunk_locations(
    const std::string& chunk_id) const {
    auto it = impl_->chunks.find(chunk_id);
    if (it == impl_->chunks.end()) {
        return std::nullopt;
    }
    return std::vector<std::string>(it->second.peer_ids.begin(),
                                    it->second.peer_ids.end());
}

std::vector<std::string> MetadataManager::get_all_chunks() const {
    std::vector<std::string> result;
    for (const auto& [chunk_id, _] : impl_->chunks) {
        result.push_back(chunk_id);
    }
    return result;
}

std::optional<ChunkIndexEntry> MetadataManager::get_chunk_metadata(
    const std::string& chunk_id) const {
    auto it = impl_->chunks.find(chunk_id);
    if (it != impl_->chunks.end()) {
        return it->second;
    }
    return std::nullopt;
}

void MetadataManager::set_chunk_metadata(const ChunkIndexEntry& entry) {
    impl_->chunks[entry.chunk_id] = entry;
}

void MetadataManager::remove_chunk_metadata(const std::string& chunk_id) {
    impl_->chunks.erase(chunk_id);
}

void MetadataManager::clear() {
    impl_->chunks.clear();
    impl_->peers.clear();
}

bool MetadataManager::load_from_file(const std::string& path) {
    Logger::instance().info("Loading metadata from: " + path);
    // Placeholder: actual implementation would load from JSON file
    return true;
}

bool MetadataManager::save_to_file(const std::string& path) const {
    Logger::instance().info("Saving metadata to: " + path);
    // Placeholder: actual implementation would save to JSON file
    return true;
}

size_t MetadataManager::total_chunks() const {
    return impl_->chunks.size();
}

size_t MetadataManager::total_peers() const {
    return impl_->peers.size();
}

} // namespace eliop2p
