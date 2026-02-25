#ifndef ELIOP2P_P2P_TRANSFER_H
#define ELIOP2P_P2P_TRANSFER_H

#include "eliop2p/base/config.h"
#include "eliop2p/p2p/node_discovery.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <elio/elio.hpp>

namespace eliop2p {

// Transfer modes for K-selection algorithm
enum class TransferMode {
    NearestFirst,   // Lowest latency first
    FastestFirst,   // Fastest peer first
    RarestFirst     // Rarest chunk first (for cache warming)
};

// Transfer request
struct TransferRequest {
    std::string chunk_id;
    std::string object_key;
    uint64_t offset = 0;              // For resume support (byte offset)
    uint64_t expected_size = 0;
    TransferMode mode = TransferMode::FastestFirst;
    uint32_t k_value = 5;              // Number of parallel connections (5-10)
    bool enable_resume = true;          // Enable resume support
    PeerList sources;                    // Candidate source peers (optional, can query from node discovery)
};

// Transfer progress
struct TransferProgress {
    std::string chunk_id;
    uint64_t bytes_transferred = 0;
    uint64_t total_bytes = 0;
    std::string current_peer;
    std::vector<std::string> active_peers;
    bool completed = false;
    bool failed = false;
    std::string error_message;
    double progress_percent = 0.0;
    double speed_bps = 0.0;
};

// Transfer statistics for a single chunk download
struct ChunkDownloadStats {
    std::string chunk_id;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint64_t bytes_downloaded = 0;
    uint32_t success_count = 0;
    uint32_t failure_count = 0;
    std::vector<std::string> used_peers;
};

// Progress callback type
using TransferProgressCallback = std::function<void(const TransferProgress&)>;

// Chunk transfer protocol constants
static constexpr uint32_t CHUNK_TRANSFER_MAGIC = 0x43484B50;  // "CHKP"
static constexpr uint32_t CHUNK_TRANSFER_VERSION = 1;

// Message types for chunk transfer protocol
enum class ChunkMessageType : uint32_t {
    Request = 1,    // Request chunk from peer
    Response = 2,  // Response with chunk data
    Error = 3,     // Error response
    Ack = 4        // Acknowledgment
};

// Chunk transfer message header
struct ChunkMessageHeader {
    uint32_t magic;              // CHUNK_TRANSFER_MAGIC
    uint32_t version;             // Protocol version
    uint32_t message_type;        // ChunkMessageType
    uint32_t chunk_id_length;     // Length of chunk_id
    uint32_t data_length;         // Length of data (0 for request)
    uint8_t hash[32];            // SHA256 hash of data
    uint32_t sequence_number;    // For multi-part transfers
    uint8_t flags;               // Flags (resume, compressed, etc.)

    // Flag constants
    static constexpr uint8_t FLAG_RESUME = 0x01;
    static constexpr uint8_t FLAG_COMPRESSED = 0x02;
    static constexpr uint8_t FLAG_LAST_PART = 0x04;
} __attribute__((packed));

// Bandwidth limiter for traffic control
class BandwidthLimiter {
public:
    BandwidthLimiter(uint64_t max_mbps);

    // Wait until bandwidth is available
    elio::coro::task<void> acquire(uint64_t bytes);

    // Update limit
    void set_limit(uint64_t mbps);

    // Get current available bandwidth
    uint64_t available_bytes() const;

private:
    uint64_t max_bytes_per_sec_;
    uint64_t available_;
    std::chrono::steady_clock::time_point last_reset_;
    mutable std::mutex mutex_;
};

// Connection state for P2P transfer
enum class ConnectionState {
    Idle,
    Connecting,
    Handshaking,
    Transferring,
    Completed,
    Failed,
    Cancelled
};

// P2P connection info
struct PeerConnection {
    std::string peer_id;
    std::string address;
    uint16_t port;
    ConnectionState state = ConnectionState::Idle;
    uint64_t bytes_transferred = 0;
    double speed_bps = 0.0;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point connect_time;
};

// Chunk transfer context (for resume support)
struct ChunkTransferContext {
    std::string chunk_id;
    std::string file_path;                     // Path to temporary file
    uint64_t total_size = 0;
    uint64_t downloaded_size = 0;
    uint64_t last_checkpoint_size = 0;         // Last flushed position
    std::chrono::steady_clock::time_point last_checkpoint_time;
    bool is_resume = false;

    // Progress checkpoint: flush every 1MB
    static constexpr uint64_t CHECKPOINT_INTERVAL = 1024 * 1024;  // 1MB
    // Chunk size: 16MB
    static constexpr uint64_t CHUNK_SIZE = 16 * 1024 * 1024;  // 16MB
};

class TransferManager {
public:
    TransferManager(const P2PConfig& config);
    ~TransferManager();

    // Start the transfer manager
    bool start();

    // Stop the transfer manager
    void stop();

    // Request chunk download from peers
    // Returns a task that completes when transfer is done
    // The data is stored in cache automatically
    elio::coro::task<std::optional<std::vector<uint8_t>>> download_chunk(
        const TransferRequest& request,
        TransferProgressCallback progress_callback = nullptr);

    // Download chunk to file (for large chunks)
    elio::coro::task<bool> download_chunk_to_file(
        const TransferRequest& request,
        const std::string& dest_path,
        TransferProgressCallback progress_callback = nullptr);

    // Upload chunk to a peer
    elio::coro::task<bool> upload_chunk(
        const std::string& chunk_id,
        const std::vector<uint8_t>& data,
        const PeerNode& target_peer);

    // Cancel ongoing transfer
    void cancel_transfer(const std::string& chunk_id);

    // Get active transfers
    std::vector<TransferProgress> get_active_transfers() const;

    // Set bandwidth limits
    void set_upload_limit(uint64_t mbps);
    void set_download_limit(uint64_t mbps);

    // Get transfer statistics
    struct TransferStats {
        uint64_t total_downloads = 0;
        uint64_t total_uploads = 0;
        uint64_t failed_downloads = 0;
        uint64_t failed_uploads = 0;
        uint64_t total_bytes_downloaded = 0;
        uint64_t total_bytes_uploaded = 0;
        uint64_t active_transfers = 0;
    };
    TransferStats get_stats() const;

    // K-selection algorithm: select K best peers based on mode
    PeerList select_k_peers(const PeerList& candidates, uint32_t k, TransferMode mode) const;

    // Save transfer progress for resume
    bool save_progress(const std::string& chunk_id, uint64_t downloaded_bytes);
    bool load_progress(const std::string& chunk_id, uint64_t& downloaded_bytes) const;
    bool has_progress(const std::string& chunk_id) const;

    // Set chunk manager for storing chunks
    class ChunkManager;
    void set_chunk_manager(ChunkManager* manager);

    // Set node discovery for querying peers
    void set_node_discovery(NodeDiscovery* discovery);

    // Get chunk transfer context
    std::shared_ptr<ChunkTransferContext> get_transfer_context(const std::string& chunk_id) const;

    // TCP server for serving chunk downloads to other peers
    uint16_t get_listen_port() const;
    elio::coro::task<void> start_tcp_server();
    void stop_tcp_server();

    // Set runtime scheduler for TCP server
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

    // Set callback for when chunk data is needed
    using ChunkDataProvider = std::function<std::optional<std::vector<uint8_t>>(const std::string& chunk_id)>;
    void set_chunk_data_provider(ChunkDataProvider provider);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_P2P_TRANSFER_H
