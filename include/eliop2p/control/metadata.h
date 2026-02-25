#ifndef ELIOP2P_CONTROL_METADATA_H
#define ELIOP2P_CONTROL_METADATA_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <optional>

namespace eliop2p {

struct ChunkIndexEntry {
    std::string chunk_id;
    std::string object_key;
    uint64_t offset;
    uint64_t size;
    std::string hash;  // SHA256
    uint64_t version;
    std::unordered_set<std::string> peer_ids;  // Nodes that have this chunk
    uint64_t last_update_time;
};

class MetadataManager {
public:
    MetadataManager();
    ~MetadataManager();

    // Update chunk location
    void update_chunk_location(const std::string& chunk_id, const std::string& peer_id);

    // Remove chunk location
    void remove_chunk_location(const std::string& chunk_id, const std::string& peer_id);

    // Get peers that have a chunk
    std::optional<std::vector<std::string>> get_chunk_locations(const std::string& chunk_id) const;

    // Get all known chunks
    std::vector<std::string> get_all_chunks() const;

    // Get chunk metadata
    std::optional<ChunkIndexEntry> get_chunk_metadata(const std::string& chunk_id) const;

    // Add or update chunk metadata
    void set_chunk_metadata(const ChunkIndexEntry& entry);

    // Remove chunk metadata
    void remove_chunk_metadata(const std::string& chunk_id);

    // Clear all metadata
    void clear();

    // Load from storage (for recovery)
    bool load_from_file(const std::string& path);

    // Persist to storage
    bool save_to_file(const std::string& path) const;

    // Get statistics - implemented inline
    size_t total_chunks() const;
    size_t total_peers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_CONTROL_METADATA_H
