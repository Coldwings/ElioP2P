#include "eliop2p/control/metadata.h"
#include "eliop2p/base/logger.h"
#include <fstream>
#include <ctime>
#include <shared_mutex>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace eliop2p {

namespace fs = std::filesystem;

struct MetadataManager::Impl {
    std::unordered_map<std::string, ChunkIndexEntry> chunks;
    std::unordered_set<std::string> peers;
    mutable std::shared_mutex mutex;
};

MetadataManager::MetadataManager()
    : impl_(std::make_unique<Impl>()) {}

MetadataManager::~MetadataManager() = default;

void MetadataManager::update_chunk_location(const std::string& chunk_id,
                                              const std::string& peer_id) {
    std::unique_lock lock(impl_->mutex);
    impl_->chunks[chunk_id].peer_ids.insert(peer_id);
    impl_->chunks[chunk_id].last_update_time = std::time(nullptr);
    impl_->peers.insert(peer_id);
}

void MetadataManager::remove_chunk_location(const std::string& chunk_id,
                                             const std::string& peer_id) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->chunks.find(chunk_id);
    if (it != impl_->chunks.end()) {
        it->second.peer_ids.erase(peer_id);
    }
}

std::optional<std::vector<std::string>> MetadataManager::get_chunk_locations(
    const std::string& chunk_id) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->chunks.find(chunk_id);
    if (it == impl_->chunks.end()) {
        return std::nullopt;
    }
    return std::vector<std::string>(it->second.peer_ids.begin(),
                                    it->second.peer_ids.end());
}

std::vector<std::string> MetadataManager::get_all_chunks() const {
    std::shared_lock lock(impl_->mutex);
    std::vector<std::string> result;
    result.reserve(impl_->chunks.size());
    for (const auto& [chunk_id, _] : impl_->chunks) {
        result.push_back(chunk_id);
    }
    return result;
}

std::optional<ChunkIndexEntry> MetadataManager::get_chunk_metadata(
    const std::string& chunk_id) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->chunks.find(chunk_id);
    if (it != impl_->chunks.end()) {
        return it->second;
    }
    return std::nullopt;
}

void MetadataManager::set_chunk_metadata(const ChunkIndexEntry& entry) {
    std::unique_lock lock(impl_->mutex);
    impl_->chunks[entry.chunk_id] = entry;
}

void MetadataManager::remove_chunk_metadata(const std::string& chunk_id) {
    std::unique_lock lock(impl_->mutex);
    impl_->chunks.erase(chunk_id);
}

void MetadataManager::clear() {
    std::unique_lock lock(impl_->mutex);
    impl_->chunks.clear();
    impl_->peers.clear();
}

bool MetadataManager::load_from_file(const std::string& path) {
    Logger::instance().info("Loading metadata from: " + path);

    if (!fs::exists(path)) {
        // No state file yet: an empty index is a valid starting point
        return true;
    }

    try {
        std::ifstream file(path);
        if (!file) {
            Logger::instance().error("Failed to open metadata file: " + path);
            return false;
        }

        nlohmann::json j;
        file >> j;

        std::unique_lock lock(impl_->mutex);
        impl_->chunks.clear();
        impl_->peers.clear();

        for (const auto& item : j.at("chunks")) {
            ChunkIndexEntry entry;
            entry.chunk_id = item.value("chunk_id", "");
            entry.object_key = item.value("object_key", "");
            entry.offset = item.value("offset", uint64_t{0});
            entry.size = item.value("size", uint64_t{0});
            entry.hash = item.value("hash", "");
            entry.version = item.value("version", uint64_t{0});
            entry.last_update_time = item.value("last_update_time", uint64_t{0});
            for (const auto& p : item.value("peer_ids", nlohmann::json::array())) {
                entry.peer_ids.insert(p.get<std::string>());
                impl_->peers.insert(p.get<std::string>());
            }
            if (!entry.chunk_id.empty()) {
                impl_->chunks[entry.chunk_id] = std::move(entry);
            }
        }

        Logger::instance().info("Loaded metadata: " +
                                std::to_string(impl_->chunks.size()) + " chunks, " +
                                std::to_string(impl_->peers.size()) + " peers");
        return true;
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to parse metadata file " + path + ": " + e.what());
        return false;
    }
}

bool MetadataManager::save_to_file(const std::string& path) const {
    Logger::instance().info("Saving metadata to: " + path);

    try {
        nlohmann::json j;
        j["chunks"] = nlohmann::json::array();
        {
            std::shared_lock lock(impl_->mutex);
            for (const auto& [id, entry] : impl_->chunks) {
                j["chunks"].push_back({
                    {"chunk_id", entry.chunk_id},
                    {"object_key", entry.object_key},
                    {"offset", entry.offset},
                    {"size", entry.size},
                    {"hash", entry.hash},
                    {"version", entry.version},
                    {"peer_ids", entry.peer_ids},
                    {"last_update_time", entry.last_update_time},
                });
            }
        }

        // Atomic write: temp file + rename so a crash mid-write cannot
        // corrupt the previous good state
        fs::path tmp_path = fs::path(path);
        tmp_path += ".tmp";
        {
            std::ofstream file(tmp_path);
            if (!file) {
                Logger::instance().error("Failed to open metadata file for writing: " + tmp_path.string());
                return false;
            }
            file << j.dump(2);
        }
        fs::rename(tmp_path, path);
        return true;
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to save metadata to " + path + ": " + e.what());
        return false;
    }
}

size_t MetadataManager::total_chunks() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->chunks.size();
}

size_t MetadataManager::total_peers() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->peers.size();
}

} // namespace eliop2p
