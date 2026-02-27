#include "eliop2p/p2p/transfer.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/config.h"
#include <elio/net/tcp.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <future>
#include <filesystem>
#include <functional>
#include <cstring>
#include <arpa/inet.h>

namespace eliop2p {

// BandwidthLimiter implementation
BandwidthLimiter::BandwidthLimiter(uint64_t max_mbps)
    : max_bytes_per_sec_(max_mbps * 1024 * 1024 / 8),
      available_(max_bytes_per_sec_),
      last_reset_(std::chrono::steady_clock::now()) {}

elio::coro::task<void> BandwidthLimiter::acquire(uint64_t bytes) {
    // Use spinlock for short critical sections
    // Note: Must release lock before co_await to avoid blocking other coroutines
    {
        elio::sync::spinlock_guard lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_reset_).count();

        if (elapsed >= 1) {
            available_ = max_bytes_per_sec_;
            last_reset_ = now;
        }

        if (available_ >= bytes) {
            available_ -= bytes;
            co_return;
        }

        // Calculate wait time while holding lock, then release
        wait_time_ms_ = ((bytes - available_) * 1000) / std::max<uint64_t>(max_bytes_per_sec_ / 1024, 1);
        available_ = 0;
    } // Lock released here

    // Wait outside the lock to not block other coroutines
    co_await elio::time::sleep_for(std::chrono::milliseconds(wait_time_ms_));
}

void BandwidthLimiter::set_limit(uint64_t mbps) {
    elio::sync::spinlock_guard lock(mutex_);
    max_bytes_per_sec_ = mbps * 1024 * 1024 / 8;
    available_ = max_bytes_per_sec_;
}

uint64_t BandwidthLimiter::available_bytes() const {
    elio::sync::spinlock_guard lock(mutex_);
    return available_;
}

// TransferManager implementation
struct TransferManager::Impl {
    P2PConfig config;
    bool running = false;
    TransferStats stats;
    std::unique_ptr<BandwidthLimiter> upload_limiter;
    std::unique_ptr<BandwidthLimiter> download_limiter;

    // Active transfers
    std::unordered_map<std::string, std::shared_ptr<ChunkTransferContext>> active_transfers;
    std::mutex transfers_mutex;

    // Progress persistence (for resume)
    std::unordered_map<std::string, uint64_t> saved_progress;
    std::string progress_dir = "/tmp/eliop2p_progress";

    // Dependencies
    class ChunkManager* chunk_manager = nullptr;
    NodeDiscovery* node_discovery = nullptr;

    // TCP server for serving chunks to other peers
    std::shared_ptr<elio::runtime::scheduler> scheduler;
    std::atomic<bool> tcp_server_running{false};
    std::thread tcp_server_thread;
    std::optional<elio::net::tcp_listener> tcp_listener;
    uint16_t listen_port = 0;
    std::atomic<bool> server_stopped{true};

    // Chunk data provider callback
    TransferManager::ChunkDataProvider chunk_data_provider;

    Impl(const P2PConfig& cfg) : config(cfg) {
        upload_limiter = std::make_unique<BandwidthLimiter>(cfg.max_upload_speed_mbps);
        download_limiter = std::make_unique<BandwidthLimiter>(cfg.max_download_speed_mbps);

        // Create progress directory
        std::filesystem::create_directories(progress_dir);
    }

    // Get or create transfer context
    std::shared_ptr<ChunkTransferContext> get_or_create_context(const std::string& chunk_id, bool resume) {
        std::lock_guard<std::mutex> lock(transfers_mutex);

        auto it = active_transfers.find(chunk_id);
        if (it != active_transfers.end()) {
            return it->second;
        }

        auto ctx = std::make_shared<ChunkTransferContext>();
        ctx->chunk_id = chunk_id;
        ctx->file_path = progress_dir + "/" + chunk_id + ".tmp";
        ctx->is_resume = resume;
        ctx->last_checkpoint_time = std::chrono::steady_clock::now();

        active_transfers[chunk_id] = ctx;
        return ctx;
    }

    // Remove completed transfer
    void remove_transfer(const std::string& chunk_id) {
        std::lock_guard<std::mutex> lock(transfers_mutex);
        active_transfers.erase(chunk_id);
    }

    // TCP server: handle incoming chunk request
    elio::coro::task<void> handle_chunk_request(elio::net::tcp_stream stream) {
        try {
            // Read request header
            ChunkMessageHeader header;
            auto read_result = co_await stream.read(&header, sizeof(header));

            if (read_result.result != sizeof(header)) {
                Logger::instance().warning("Invalid chunk request header size");
                co_return;
            }

            // Validate magic
            if (header.magic != CHUNK_TRANSFER_MAGIC) {
                Logger::instance().warning("Invalid magic in chunk request");
                co_return;
            }

            // Read chunk_id
            if (header.chunk_id_length > 256) {
                Logger::instance().warning("Chunk ID too long");
                co_return;
            }

            std::vector<char> chunk_id_buf(header.chunk_id_length);
            auto id_result = co_await stream.read(chunk_id_buf.data(), header.chunk_id_length);
            if (id_result.result != static_cast<ssize_t>(header.chunk_id_length)) {
                Logger::instance().warning("Failed to read chunk ID");
                co_return;
            }

            std::string chunk_id(chunk_id_buf.begin(), chunk_id_buf.end());
            Logger::instance().debug("Chunk request for: " + chunk_id);

            // Check if we have this chunk
            std::optional<std::vector<uint8_t>> chunk_data;
            if (chunk_data_provider) {
                chunk_data = chunk_data_provider(chunk_id);
            }

            if (!chunk_data || chunk_data->empty()) {
                // Send error response
                ChunkMessageHeader resp_header{};
                resp_header.magic = CHUNK_TRANSFER_MAGIC;
                resp_header.version = CHUNK_TRANSFER_VERSION;
                resp_header.message_type = static_cast<uint32_t>(ChunkMessageType::Error);
                resp_header.chunk_id_length = header.chunk_id_length;
                resp_header.data_length = 0;
                resp_header.flags = 0;

                auto write_result = co_await stream.write(&resp_header, sizeof(resp_header));
                if (write_result.result == sizeof(resp_header)) {
                    co_await stream.write(chunk_id_buf.data(), header.chunk_id_length);
                }
                Logger::instance().warning("Chunk not found: " + chunk_id);
                co_await stream.close();
                co_return;
            }

            // Calculate hash
            // Note: In real implementation, use proper SHA256
            // For now, we'll use a placeholder

            // Send response with chunk data
            ChunkMessageHeader resp_header{};
            resp_header.magic = CHUNK_TRANSFER_MAGIC;
            resp_header.version = CHUNK_TRANSFER_VERSION;
            resp_header.message_type = static_cast<uint32_t>(ChunkMessageType::Response);
            resp_header.chunk_id_length = header.chunk_id_length;
            resp_header.data_length = static_cast<uint32_t>(chunk_data->size());
            resp_header.sequence_number = 0;
            resp_header.flags = ChunkMessageHeader::FLAG_LAST_PART;

            // Send header
            auto hdr_result = co_await stream.write(&resp_header, sizeof(resp_header));
            if (hdr_result.result != sizeof(resp_header)) {
                Logger::instance().error("Failed to send response header");
                co_await stream.close();
                co_return;
            }
            // Send chunk_id
            auto id_send_result = co_await stream.write(chunk_id_buf.data(), header.chunk_id_length);
            if (id_send_result.result != static_cast<ssize_t>(header.chunk_id_length)) {
                Logger::instance().error("Failed to send chunk_id");
                co_await stream.close();
                co_return;
            }
            // Send data
            auto data_result = co_await stream.write(chunk_data->data(), chunk_data->size());
            if (data_result.result != static_cast<ssize_t>(chunk_data->size())) {
                Logger::instance().error("Failed to send chunk data");
                co_await stream.close();
                co_return;
            }

            co_await stream.close();
            Logger::instance().info("Sent chunk " + chunk_id + " (" + std::to_string(chunk_data->size()) + " bytes)");

        } catch (const std::exception& e) {
            Logger::instance().error("Error handling chunk request: " + std::string(e.what()));
        }
        co_return;
    }

    // TCP server: accept and handle connections
    elio::coro::task<void> tcp_server_loop() {
        Logger::instance().info("TCP chunk server started on port " + std::to_string(listen_port));

        while (tcp_server_running) {
            try {
                auto stream_result = co_await tcp_listener->accept();
                if (!stream_result) {
                    continue;
                }

                // Handle request in background
                (void)handle_chunk_request(std::move(*stream_result)).spawn();

            } catch (const std::exception& e) {
                if (tcp_server_running) {
                    Logger::instance().error("TCP server error: " + std::string(e.what()));
                }
            }
        }

        co_return;
    }
};

TransferManager::TransferManager(const P2PConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

TransferManager::~TransferManager() {
    stop();
}

bool TransferManager::start() {
    impl_->running = true;
    Logger::instance().info("Transfer manager started");
    return true;
}

void TransferManager::stop() {
    // Stop TCP server first
    stop_tcp_server();

    impl_->running = false;
    Logger::instance().info("Transfer manager stopped");
}

uint16_t TransferManager::get_listen_port() const {
    return impl_->listen_port;
}

elio::coro::task<void> TransferManager::start_tcp_server() {
    if (impl_->tcp_server_running) {
        Logger::instance().warning("TCP server already running");
        co_return;
    }

    if (!impl_->scheduler) {
        // Create default scheduler if not set
        impl_->scheduler = std::make_shared<elio::runtime::scheduler>(2);
    }

    // Create and bind TCP listener
    elio::net::tcp_options opts;
    opts.reuse_addr = true;
    opts.no_delay = true;

    elio::net::ipv4_address addr("0.0.0.0", impl_->config.listen_port);
    auto listener = elio::net::tcp_listener::bind(addr, opts);

    if (!listener) {
        Logger::instance().error("Failed to bind TCP listener on port " + std::to_string(impl_->config.listen_port));
        co_return;
    }

    impl_->tcp_listener = std::move(listener);
    impl_->listen_port = impl_->tcp_listener->local_address().port();
    impl_->tcp_server_running = true;
    impl_->server_stopped = false;

    Logger::instance().info("TCP chunk server listening on port " + std::to_string(impl_->listen_port));

    // Run server loop in the scheduler
    (void)impl_->tcp_server_loop().spawn();

    co_return;
}

void TransferManager::stop_tcp_server() {
    if (!impl_->tcp_server_running) {
        return;
    }

    Logger::instance().info("Stopping TCP chunk server...");
    impl_->tcp_server_running = false;

    if (impl_->tcp_listener) {
        impl_->tcp_listener->close();
        impl_->tcp_listener = std::nullopt;
    }

    // Wait for server to stop
    while (!impl_->server_stopped.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Logger::instance().info("TCP chunk server stopped");
}

void TransferManager::set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) {
    impl_->scheduler = std::move(scheduler);
}

void TransferManager::set_chunk_data_provider(ChunkDataProvider provider) {
    impl_->chunk_data_provider = std::move(provider);
}

// K-selection algorithm: select K best peers based on transfer mode
PeerList TransferManager::select_k_peers(const PeerList& candidates, uint32_t k, TransferMode mode) const {
    if (candidates.empty()) {
        return {};
    }

    PeerList result;
    uint32_t actual_k = std::min(k, static_cast<uint32_t>(candidates.size()));

    switch (mode) {
        case TransferMode::NearestFirst: {
            // Sort by latency (ascending)
            PeerList sorted = candidates;
            std::sort(sorted.begin(), sorted.end(),
                [](const PeerNode& a, const PeerNode& b) {
                    return a.latency_ms < b.latency_ms;
                });
            result.assign(sorted.begin(), sorted.begin() + actual_k);
            break;
        }
        case TransferMode::FastestFirst: {
            // Sort by throughput (descending)
            PeerList sorted = candidates;
            std::sort(sorted.begin(), sorted.end(),
                [](const PeerNode& a, const PeerNode& b) {
                    return a.throughput_mbps > b.throughput_mbps;
                });
            result.assign(sorted.begin(), sorted.begin() + actual_k);
            break;
        }
        case TransferMode::RarestFirst: {
            // For Rarest-First, we need to know chunk rarity
            // Peers with rarer chunks are prioritized
            if (impl_->node_discovery) {
                // Get rarity for each peer
                PeerList sorted = candidates;
                std::sort(sorted.begin(), sorted.end(),
                    [this](const PeerNode& a, const PeerNode& b) {
                        uint32_t rarity_a = 0, rarity_b = 0;
                        // Count how many peers have the chunks this peer has
                        for (const auto& chunk : a.available_chunks) {
                            rarity_a += impl_->node_discovery->get_chunk_rarity(chunk);
                        }
                        for (const auto& chunk : b.available_chunks) {
                            rarity_b += impl_->node_discovery->get_chunk_rarity(chunk);
                        }
                        // Fewer peers = higher priority (lower rarity score)
                        return rarity_a < rarity_b;
                    });
                result.assign(sorted.begin(), sorted.begin() + actual_k);
            } else {
                // Fallback to random selection
                result = candidates;
                std::shuffle(result.begin(), result.end(), std::mt19937{std::random_device{}()});
                result.resize(actual_k);
            }
            break;
        }
        default:
            result.assign(candidates.begin(), candidates.begin() + actual_k);
    }

    return result;
}

// Helper function: download from a single peer with failure handling
// Returns: pair<success, bytes_downloaded>
elio::coro::task<std::pair<bool, uint64_t>> download_from_peer(
    BandwidthLimiter* download_limiter,
    std::shared_ptr<ChunkTransferContext> ctx,
    const PeerNode& peer,
    std::vector<uint8_t>& result_buffer,
    TransferProgress& progress,
    TransferProgressCallback progress_callback,
    bool& running) {

    uint64_t downloaded_from_peer = 0;
    const uint64_t chunk_size = 256 * 1024;  // 256KB chunks

    try {
        Logger::instance().debug("Starting TCP download from peer: " + peer.node_id + " at " + peer.address + ":" + std::to_string(peer.port));

        // Establish TCP connection to peer
        elio::net::tcp_options opts;
        opts.no_delay = true;

        auto connect_result = co_await elio::net::tcp_connect(peer.address, peer.port, opts);
        if (!connect_result) {
            Logger::instance().error("Failed to connect to peer: " + peer.node_id);
            co_return std::make_pair(false, 0);
        }

        elio::net::tcp_stream& stream = *connect_result;

        // Build and send request
        std::string chunk_id = ctx->chunk_id;

        // Build request header
        ChunkMessageHeader header{};
        header.magic = CHUNK_TRANSFER_MAGIC;
        header.version = CHUNK_TRANSFER_VERSION;
        header.message_type = static_cast<uint32_t>(ChunkMessageType::Request);
        header.chunk_id_length = static_cast<uint32_t>(chunk_id.size());
        header.data_length = 0;  // No data in request
        header.sequence_number = 0;
        header.flags = ctx->is_resume ? ChunkMessageHeader::FLAG_RESUME : 0;

        // Send header
        auto write_result = co_await stream.write(&header, sizeof(header));
        if (write_result.result != sizeof(header)) {
            Logger::instance().error("Failed to send request header to peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Send chunk_id
        auto id_result = co_await stream.write(chunk_id.data(), chunk_id.size());
        if (id_result.result != static_cast<ssize_t>(chunk_id.size())) {
            Logger::instance().error("Failed to send chunk_id to peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Read response header
        ChunkMessageHeader resp_header;
        auto read_result = co_await stream.read(&resp_header, sizeof(resp_header));
        if (read_result.result != sizeof(resp_header)) {
            Logger::instance().error("Failed to read response header from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Validate response
        if (resp_header.magic != CHUNK_TRANSFER_MAGIC) {
            Logger::instance().error("Invalid magic in response from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        if (resp_header.message_type == static_cast<uint32_t>(ChunkMessageType::Error)) {
            Logger::instance().error("Peer returned error for chunk: " + ctx->chunk_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        if (resp_header.message_type != static_cast<uint32_t>(ChunkMessageType::Response)) {
            Logger::instance().error("Unexpected message type from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Read chunk data
        uint64_t total_data_length = resp_header.data_length;
        result_buffer.reserve(total_data_length);

        uint64_t offset = 0;
        while (offset < total_data_length) {
            if (!running) {
                Logger::instance().warning("Transfer cancelled");
                co_await stream.close();
                co_return std::make_pair(false, downloaded_from_peer);
            }

            // Apply bandwidth limiting
            if (download_limiter) {
                uint64_t bytes_to_transfer = std::min(chunk_size, total_data_length - offset);
                co_await download_limiter->acquire(bytes_to_transfer);
            }

            // Read data from TCP
            uint64_t read_size = std::min(chunk_size, total_data_length - offset);
            std::vector<uint8_t> chunk_data(read_size);
            auto data_result = co_await stream.read(chunk_data.data(), read_size);

            if (data_result.result <= 0) {
                Logger::instance().error("Connection closed while reading data from peer: " + peer.node_id);
                co_await stream.close();
                co_return std::make_pair(false, downloaded_from_peer);
            }

            result_buffer.insert(result_buffer.end(), chunk_data.begin(), chunk_data.begin() + data_result.result);
            downloaded_from_peer += data_result.result;
            offset += data_result.result;

            ctx->downloaded_size += data_result.result;
            progress.bytes_transferred = ctx->downloaded_size;
            progress.progress_percent = (double)ctx->downloaded_size / ctx->total_size * 100.0;
            progress.current_peer = peer.node_id;

            // Report progress
            if (progress_callback) {
                progress_callback(progress);
            }

            // Checkpoint every 1MB
            if (ctx->downloaded_size - ctx->last_checkpoint_size >= ChunkTransferContext::CHECKPOINT_INTERVAL) {
                ctx->last_checkpoint_size = ctx->downloaded_size;
                Logger::instance().debug("Checkpoint saved at " + std::to_string(ctx->downloaded_size) + " bytes");
            }
        }

        // Close socket
        co_await stream.close();

        Logger::instance().info("Successfully downloaded " + std::to_string(downloaded_from_peer) +
                               " bytes from peer: " + peer.node_id);
        co_return std::make_pair(true, downloaded_from_peer);

    } catch (const std::exception& e) {
        Logger::instance().error("Download failed from peer " + peer.node_id + ": " + e.what());
        co_return std::make_pair(false, downloaded_from_peer);
    } catch (...) {
        Logger::instance().error("Unknown error downloading from peer: " + peer.node_id);
        co_return std::make_pair(false, downloaded_from_peer);
    }
}

// Helper function: download to file from a single peer with failure handling
// Returns: pair<success, bytes_downloaded>
elio::coro::task<std::pair<bool, uint64_t>> download_from_peer_to_file(
    BandwidthLimiter* download_limiter,
    std::shared_ptr<ChunkTransferContext> ctx,
    const PeerNode& peer,
    std::ofstream& out_file,
    TransferProgress& progress,
    TransferProgressCallback progress_callback,
    bool& running) {

    uint64_t downloaded_from_peer = 0;
    const uint64_t chunk_size = 256 * 1024;  // 256KB chunks

    try {
        Logger::instance().debug("Starting TCP file download from peer: " + peer.node_id + " at " + peer.address + ":" + std::to_string(peer.port));

        // Establish TCP connection to peer
        elio::net::tcp_options opts;
        opts.no_delay = true;

        auto connect_result = co_await elio::net::tcp_connect(peer.address, peer.port, opts);
        if (!connect_result) {
            Logger::instance().error("Failed to connect to peer for file download: " + peer.node_id);
            co_return std::make_pair(false, 0);
        }

        elio::net::tcp_stream& stream = *connect_result;

        // Build and send request
        std::string chunk_id = ctx->chunk_id;

        // Build request header
        ChunkMessageHeader header{};
        header.magic = CHUNK_TRANSFER_MAGIC;
        header.version = CHUNK_TRANSFER_VERSION;
        header.message_type = static_cast<uint32_t>(ChunkMessageType::Request);
        header.chunk_id_length = static_cast<uint32_t>(chunk_id.size());
        header.data_length = 0;  // No data in request
        header.sequence_number = 0;
        header.flags = ctx->is_resume ? ChunkMessageHeader::FLAG_RESUME : 0;

        // Send header
        auto write_result = co_await stream.write(&header, sizeof(header));
        if (write_result.result != sizeof(header)) {
            Logger::instance().error("Failed to send request header to peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Send chunk_id
        auto id_result = co_await stream.write(chunk_id.data(), chunk_id.size());
        if (id_result.result != static_cast<ssize_t>(chunk_id.size())) {
            Logger::instance().error("Failed to send chunk_id to peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Read response header
        ChunkMessageHeader resp_header;
        auto read_result = co_await stream.read(&resp_header, sizeof(resp_header));
        if (read_result.result != sizeof(resp_header)) {
            Logger::instance().error("Failed to read response header from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Validate response
        if (resp_header.magic != CHUNK_TRANSFER_MAGIC) {
            Logger::instance().error("Invalid magic in response from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        if (resp_header.message_type == static_cast<uint32_t>(ChunkMessageType::Error)) {
            Logger::instance().error("Peer returned error for chunk: " + ctx->chunk_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        if (resp_header.message_type != static_cast<uint32_t>(ChunkMessageType::Response)) {
            Logger::instance().error("Unexpected message type from peer: " + peer.node_id);
            co_await stream.close();
            co_return std::make_pair(false, downloaded_from_peer);
        }

        // Read chunk data and write to file
        uint64_t total_data_length = resp_header.data_length;
        uint64_t offset = 0;

        while (offset < total_data_length) {
            if (!running) {
                Logger::instance().warning("Transfer cancelled");
                co_await stream.close();
                co_return std::make_pair(false, downloaded_from_peer);
            }

            // Apply bandwidth limiting
            if (download_limiter) {
                uint64_t bytes_to_transfer = std::min(chunk_size, total_data_length - offset);
                co_await download_limiter->acquire(bytes_to_transfer);
            }

            // Read data from TCP
            uint64_t read_size = std::min(chunk_size, total_data_length - offset);
            std::vector<uint8_t> buffer(read_size);
            auto data_result = co_await stream.read(buffer.data(), read_size);

            if (data_result.result <= 0) {
                Logger::instance().error("Connection closed while reading data from peer: " + peer.node_id);
                co_await stream.close();
                co_return std::make_pair(false, downloaded_from_peer);
            }

            // Write to file
            out_file.write(reinterpret_cast<const char*>(buffer.data()), data_result.result);
            downloaded_from_peer += data_result.result;
            offset += data_result.result;

            ctx->downloaded_size += data_result.result;
            progress.bytes_transferred = ctx->downloaded_size;
            progress.progress_percent = (double)ctx->downloaded_size / ctx->total_size * 100.0;
            progress.current_peer = peer.node_id;

            // Report progress
            if (progress_callback) {
                progress_callback(progress);
            }

            // Flush every 1MB
            if (ctx->downloaded_size - ctx->last_checkpoint_size >= ChunkTransferContext::CHECKPOINT_INTERVAL) {
                out_file.flush();
                ctx->last_checkpoint_size = ctx->downloaded_size;
                Logger::instance().debug("Checkpoint saved at " + std::to_string(ctx->downloaded_size) + " bytes");
            }
        }

        // Final flush
        out_file.flush();

        // Close socket
        co_await stream.close();

        Logger::instance().info("Successfully downloaded " + std::to_string(downloaded_from_peer) +
                               " bytes to file from peer: " + peer.node_id);
        co_return std::make_pair(true, downloaded_from_peer);

    } catch (const std::exception& e) {
        Logger::instance().error("File download failed from peer " + peer.node_id + ": " + e.what());
        co_return std::make_pair(false, downloaded_from_peer);
    } catch (...) {
        Logger::instance().error("Unknown error downloading file from peer: " + peer.node_id);
        co_return std::make_pair(false, downloaded_from_peer);
    }
}

elio::coro::task<std::optional<std::vector<uint8_t>>> TransferManager::download_chunk(
    const TransferRequest& request,
    TransferProgressCallback progress_callback) {

    if (!impl_->running) {
        co_return std::nullopt;
    }

    Logger::instance().info("Starting download for chunk: " + request.chunk_id +
                            ", mode: " + std::to_string(static_cast<int>(request.mode)) +
                            ", K: " + std::to_string(request.k_value));

    impl_->stats.total_downloads++;

    // Get transfer context
    auto ctx = impl_->get_or_create_context(request.chunk_id, request.enable_resume);

    // Determine if we're resuming
    if (request.enable_resume) {
        uint64_t saved_offset = 0;
        if (load_progress(request.chunk_id, saved_offset) && saved_offset > 0) {
            ctx->is_resume = true;
            ctx->downloaded_size = saved_offset;
            Logger::instance().info("Resuming chunk download from offset: " + std::to_string(saved_offset));
        }
    }

    // Set total size
    ctx->total_size = request.expected_size > 0 ? request.expected_size : ChunkTransferContext::CHUNK_SIZE;

    // Prepare result buffer
    std::vector<uint8_t> result;
    result.reserve(ctx->total_size);

    // Get candidate peers (or use provided sources)
    PeerList candidates;
    if (!request.sources.empty()) {
        candidates = request.sources;
    } else if (impl_->node_discovery) {
        candidates = impl_->node_discovery->get_peers_with_chunk(request.chunk_id);
    }

    if (candidates.empty()) {
        Logger::instance().warning("No peers available for chunk: " + request.chunk_id);
        impl_->stats.failed_downloads++;
        co_return std::nullopt;
    }

    // K-selection: choose K best peers
    uint32_t k = request.k_value > 0 ? request.k_value : impl_->config.selection_k;
    PeerList selected_peers = select_k_peers(candidates, k, request.mode);

    Logger::instance().info("Selected " + std::to_string(selected_peers.size()) +
                            " peers for parallel download");

    // TRUE PARALLEL DOWNLOAD: spawn tasks for all selected peers concurrently
    // Each peer downloads the entire chunk (or a portion) in parallel
    // If one peer fails, we can switch to backup peers

    TransferProgress progress;
    progress.chunk_id = request.chunk_id;
    progress.total_bytes = ctx->total_size;

    bool download_success = false;
    bool transfer_running = true;

    // Create parallel download tasks for all selected peers
    std::vector<elio::coro::join_handle<std::pair<bool, uint64_t>>> download_tasks;

    for (const auto& peer : selected_peers) {
        progress.active_peers.push_back(peer.node_id);
    }

    // Launch all peer downloads in parallel using spawn()
    for (size_t i = 0; i < selected_peers.size(); ++i) {
        const auto& peer = selected_peers[i];
        auto download_task = download_from_peer(
            impl_->download_limiter.get(),
            ctx,
            peer,
            result,
            progress,
            progress_callback,
            transfer_running
        ).spawn();
        download_tasks.push_back(std::move(download_task));
    }

    // Wait for all parallel downloads to complete
    // Using a simple approach: wait for first to succeed, or all to fail
    uint64_t successful_peers = 0;

    for (auto& task : download_tasks) {
        auto [success, bytes_downloaded] = co_await task;
        if (success) {
            successful_peers++;
            download_success = true;
            Logger::instance().info("Parallel download succeeded from one peer, " +
                                   std::to_string(successful_peers) + " total successful");
        }
    }

    // If no peer succeeded, try fallback peers
    if (!download_success) {
        Logger::instance().warning("All primary peers failed, trying fallback peers");

        // Get additional fallback peers (exclude already tried ones)
        PeerList fallback_peers;
        for (const auto& candidate : candidates) {
            bool already_tried = false;
            for (const auto& tried : selected_peers) {
                if (candidate.node_id == tried.node_id) {
                    already_tried = true;
                    break;
                }
            }
            if (!already_tried) {
                fallback_peers.push_back(candidate);
            }
        }

        // Try fallback peers one by one with failure handling
        for (const auto& peer : fallback_peers) {
            if (!impl_->running) {
                break;
            }

            Logger::instance().info("Trying fallback peer: " + peer.node_id);

            try {
                auto [success, bytes_downloaded] = co_await download_from_peer(
                    impl_->download_limiter.get(),
                    ctx,
                    peer,
                    result,
                    progress,
                    progress_callback,
                    transfer_running
                );

                if (success) {
                    download_success = true;
                    successful_peers++;
                    Logger::instance().info("Fallback peer succeeded: " + peer.node_id);
                    break;
                }
            } catch (const std::exception& e) {
                Logger::instance().error("Fallback peer failed: " + std::string(e.what()));
                // Continue to next fallback peer
            }
        }
    }

    if (!download_success) {
        Logger::instance().error("All peers failed for chunk: " + request.chunk_id);
        impl_->stats.failed_downloads++;
        co_return std::nullopt;
    }

    // Final checkpoint
    save_progress(request.chunk_id, ctx->downloaded_size);

    // Clean up progress file on success
    std::filesystem::remove(ctx->file_path);
    impl_->remove_transfer(request.chunk_id);

    progress.completed = true;
    progress.bytes_transferred = ctx->downloaded_size;
    progress.progress_percent = 100.0;

    if (progress_callback) {
        progress_callback(progress);
    }

    impl_->stats.total_bytes_downloaded += result.size();

    Logger::instance().info("Download completed for chunk: " + request.chunk_id +
                            ", size: " + std::to_string(result.size()) + " bytes, " +
                            std::to_string(successful_peers) + " successful peer(s)");

    co_return result;
}

elio::coro::task<bool> TransferManager::download_chunk_to_file(
    const TransferRequest& request,
    const std::string& dest_path,
    TransferProgressCallback progress_callback) {

    if (!impl_->running) {
        co_return false;
    }

    Logger::instance().info("Downloading chunk to file: " + request.chunk_id + " -> " + dest_path);

    // Get transfer context
    auto ctx = impl_->get_or_create_context(request.chunk_id, request.enable_resume);
    ctx->file_path = dest_path;
    ctx->total_size = request.expected_size > 0 ? request.expected_size : ChunkTransferContext::CHUNK_SIZE;

    // Check for resume
    uint64_t saved_offset = 0;
    if (request.enable_resume && load_progress(request.chunk_id, saved_offset) && saved_offset > 0) {
        ctx->is_resume = true;
        ctx->downloaded_size = saved_offset;
    }

    // Open file for writing
    std::ofstream out_file;
    if (ctx->is_resume) {
        out_file.open(dest_path, std::ios::app | std::ios::binary);
    } else {
        out_file.open(dest_path, std::ios::out | std::ios::binary);
    }

    if (!out_file.is_open()) {
        Logger::instance().error("Failed to open file for writing: " + dest_path);
        co_return false;
    }

    // Get candidates and select K peers
    PeerList candidates;
    if (!request.sources.empty()) {
        candidates = request.sources;
    } else if (impl_->node_discovery) {
        candidates = impl_->node_discovery->get_peers_with_chunk(request.chunk_id);
    }

    if (candidates.empty()) {
        Logger::instance().warning("No peers available for chunk: " + request.chunk_id);
        co_return false;
    }

    uint32_t k = request.k_value > 0 ? request.k_value : impl_->config.selection_k;
    PeerList selected_peers = select_k_peers(candidates, k, request.mode);

    Logger::instance().info("Selected " + std::to_string(selected_peers.size()) +
                            " peers for parallel file download");

    TransferProgress progress;
    progress.chunk_id = request.chunk_id;
    progress.total_bytes = ctx->total_size;

    bool download_success = false;
    bool transfer_running = true;
    uint64_t successful_peers = 0;

    // TRUE PARALLEL DOWNLOAD: spawn tasks for all selected peers concurrently
    // Each peer downloads the entire chunk (or a portion) in parallel
    // If one peer fails, we can switch to backup peers

    for (const auto& peer : selected_peers) {
        progress.active_peers.push_back(peer.node_id);
    }

    // Launch all peer downloads in parallel using spawn()
    std::vector<elio::coro::join_handle<std::pair<bool, uint64_t>>> download_tasks;

    for (size_t i = 0; i < selected_peers.size(); ++i) {
        const auto& peer = selected_peers[i];
        auto download_task = download_from_peer_to_file(
            impl_->download_limiter.get(),
            ctx,
            peer,
            out_file,
            progress,
            progress_callback,
            transfer_running
        ).spawn();
        download_tasks.push_back(std::move(download_task));
    }

    // Wait for all parallel downloads to complete
    for (auto& task : download_tasks) {
        auto [success, bytes_downloaded] = co_await task;
        if (success) {
            successful_peers++;
            download_success = true;
            Logger::instance().info("Parallel file download succeeded from one peer, " +
                                   std::to_string(successful_peers) + " total successful");
        }
    }

    // If no peer succeeded, try fallback peers
    if (!download_success) {
        Logger::instance().warning("All primary peers failed, trying fallback peers");

        // Get additional fallback peers (exclude already tried ones)
        PeerList fallback_peers;
        for (const auto& candidate : candidates) {
            bool already_tried = false;
            for (const auto& tried : selected_peers) {
                if (candidate.node_id == tried.node_id) {
                    already_tried = true;
                    break;
                }
            }
            if (!already_tried) {
                fallback_peers.push_back(candidate);
            }
        }

        // Try fallback peers one by one with failure handling
        for (const auto& peer : fallback_peers) {
            if (!impl_->running) {
                break;
            }

            Logger::instance().info("Trying fallback peer for file: " + peer.node_id);

            try {
                auto [success, bytes_downloaded] = co_await download_from_peer_to_file(
                    impl_->download_limiter.get(),
                    ctx,
                    peer,
                    out_file,
                    progress,
                    progress_callback,
                    transfer_running
                );

                if (success) {
                    download_success = true;
                    successful_peers++;
                    Logger::instance().info("Fallback peer succeeded for file: " + peer.node_id);
                    break;
                }
            } catch (const std::exception& e) {
                Logger::instance().error("Fallback peer failed for file: " + std::string(e.what()));
                // Continue to next fallback peer
            }
        }
    }

    if (!download_success) {
        Logger::instance().error("All peers failed for chunk file download: " + request.chunk_id);
        out_file.close();
        co_return false;
    }

    // Final flush and checkpoint
    out_file.flush();
    // Note: In production, you'd want to call fsync here for durability
    // This requires opening the file with a separate file descriptor
    save_progress(request.chunk_id, ctx->downloaded_size);
    out_file.close();

    // Clean up progress
    std::filesystem::remove(ctx->file_path + ".progress");
    impl_->remove_transfer(request.chunk_id);

    progress.completed = true;
    impl_->stats.total_bytes_downloaded += ctx->downloaded_size;

    if (progress_callback) {
        progress_callback(progress);
    }

    Logger::instance().info("File download completed for chunk: " + request.chunk_id +
                            ", size: " + std::to_string(ctx->downloaded_size) + " bytes, " +
                            std::to_string(successful_peers) + " successful peer(s)");

    co_return true;
}

elio::coro::task<bool> TransferManager::upload_chunk(
    const std::string& chunk_id,
    const std::vector<uint8_t>& data,
    const PeerNode& target_peer) {

    if (!impl_->running) {
        co_return false;
    }

    Logger::instance().info("Uploading chunk: " + chunk_id + " to " + target_peer.node_id +
                           " at " + target_peer.address + ":" + std::to_string(target_peer.port));

    impl_->stats.total_uploads++;

    try {
        // Establish TCP connection to target peer
        elio::net::tcp_options opts;
        opts.no_delay = true;

        auto connect_result = co_await elio::net::tcp_connect(target_peer.address, target_peer.port, opts);
        if (!connect_result) {
            Logger::instance().error("Failed to connect to peer for upload: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_return false;
        }

        elio::net::tcp_stream& stream = *connect_result;

        // Build request header
        ChunkMessageHeader header{};
        header.magic = CHUNK_TRANSFER_MAGIC;
        header.version = CHUNK_TRANSFER_VERSION;
        header.message_type = static_cast<uint32_t>(ChunkMessageType::Request);
        header.chunk_id_length = static_cast<uint32_t>(chunk_id.size());
        header.data_length = static_cast<uint32_t>(data.size());
        header.sequence_number = 0;
        header.flags = ChunkMessageHeader::FLAG_LAST_PART;

        // Send header
        auto write_result = co_await stream.write(&header, sizeof(header));
        if (write_result.result != sizeof(header)) {
            Logger::instance().error("Failed to send upload request header to peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // Send chunk_id
        auto id_result = co_await stream.write(chunk_id.data(), chunk_id.size());
        if (id_result.result != static_cast<ssize_t>(chunk_id.size())) {
            Logger::instance().error("Failed to send chunk_id to peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // Upload data in chunks with bandwidth limiting
        const uint64_t chunk_size = 256 * 1024;  // 256KB chunks
        uint64_t offset = 0;

        while (offset < data.size()) {
            // Apply bandwidth limiting before data transfer
            if (impl_->upload_limiter) {
                uint64_t bytes_to_transfer = std::min(chunk_size, static_cast<uint64_t>(data.size()) - offset);
                co_await impl_->upload_limiter->acquire(bytes_to_transfer);
            }

            // Send data chunk
            uint64_t bytes_this_chunk = std::min(chunk_size, static_cast<uint64_t>(data.size()) - offset);
            auto data_result = co_await stream.write(data.data() + offset, bytes_this_chunk);

            if (data_result.result <= 0) {
                Logger::instance().error("Connection closed while uploading to peer: " + target_peer.node_id);
                impl_->stats.failed_uploads++;
                co_await stream.close();
                co_return false;
            }

            offset += data_result.result;
        }

        // Read response
        ChunkMessageHeader resp_header;
        auto read_result = co_await stream.read(&resp_header, sizeof(resp_header));

        if (read_result.result != sizeof(resp_header)) {
            Logger::instance().error("Failed to read upload response from peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // Validate response
        if (resp_header.magic != CHUNK_TRANSFER_MAGIC) {
            Logger::instance().error("Invalid magic in upload response from peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        if (resp_header.message_type == static_cast<uint32_t>(ChunkMessageType::Error)) {
            Logger::instance().error("Peer returned error for upload: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // Close socket
        co_await stream.close();

        impl_->stats.total_bytes_uploaded += data.size();
        Logger::instance().info("Upload completed: " + std::to_string(data.size()) +
                               " bytes to " + target_peer.node_id);

        co_return true;

    } catch (const std::exception& e) {
        Logger::instance().error("Upload failed: " + std::string(e.what()));
        impl_->stats.failed_uploads++;
        co_return false;
    }
}

void TransferManager::cancel_transfer(const std::string& chunk_id) {
    Logger::instance().info("Cancelling transfer: " + chunk_id);

    std::lock_guard<std::mutex> lock(impl_->transfers_mutex);
    auto it = impl_->active_transfers.find(chunk_id);
    if (it != impl_->active_transfers.end()) {
        // Save progress before cancellation for resume
        save_progress(chunk_id, it->second->downloaded_size);
    }
}

std::vector<TransferProgress> TransferManager::get_active_transfers() const {
    std::vector<TransferProgress> result;

    std::lock_guard<std::mutex> lock(impl_->transfers_mutex);
    for (const auto& [chunk_id, ctx] : impl_->active_transfers) {
        TransferProgress progress;
        progress.chunk_id = chunk_id;
        progress.bytes_transferred = ctx->downloaded_size;
        progress.total_bytes = ctx->total_size;
        progress.progress_percent = ctx->total_size > 0 ?
            (double)ctx->downloaded_size / ctx->total_size * 100.0 : 0.0;
        result.push_back(progress);
    }

    return result;
}

void TransferManager::set_upload_limit(uint64_t mbps) {
    impl_->upload_limiter->set_limit(mbps);
    impl_->config.max_upload_speed_mbps = mbps;
    Logger::instance().info("Upload limit set to " + std::to_string(mbps) + " Mbps");
}

void TransferManager::set_download_limit(uint64_t mbps) {
    impl_->download_limiter->set_limit(mbps);
    impl_->config.max_download_speed_mbps = mbps;
    Logger::instance().info("Download limit set to " + std::to_string(mbps) + " Mbps");
}

TransferManager::TransferStats TransferManager::get_stats() const {
    TransferStats stats = impl_->stats;
    std::lock_guard<std::mutex> lock(impl_->transfers_mutex);
    stats.active_transfers = static_cast<uint64_t>(impl_->active_transfers.size());
    return stats;
}

bool TransferManager::save_progress(const std::string& chunk_id, uint64_t downloaded_bytes) {
    try {
        std::string progress_file = impl_->progress_dir + "/" + chunk_id + ".progress";
        std::ofstream out(progress_file);
        if (out.is_open()) {
            out << downloaded_bytes << "\n";
            out.close();
            impl_->saved_progress[chunk_id] = downloaded_bytes;
            return true;
        }
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to save progress: " + std::string(e.what()));
    }
    return false;
}

bool TransferManager::load_progress(const std::string& chunk_id, uint64_t& downloaded_bytes) const {
    // Check in-memory first
    auto it = impl_->saved_progress.find(chunk_id);
    if (it != impl_->saved_progress.end()) {
        downloaded_bytes = it->second;
        return true;
    }

    // Try to load from file
    try {
        std::string progress_file = impl_->progress_dir + "/" + chunk_id + ".progress";
        std::ifstream in(progress_file);
        if (in.is_open()) {
            in >> downloaded_bytes;
            in.close();
            return downloaded_bytes > 0;
        }
    } catch (const std::exception& e) {
        Logger::instance().debug("No saved progress found for chunk: " + chunk_id);
    }
    return false;
}

bool TransferManager::has_progress(const std::string& chunk_id) const {
    uint64_t progress = 0;
    return load_progress(chunk_id, progress) && progress > 0;
}

void TransferManager::set_chunk_manager(ChunkManager* manager) {
    impl_->chunk_manager = manager;
}

void TransferManager::set_node_discovery(NodeDiscovery* discovery) {
    impl_->node_discovery = discovery;
}

std::shared_ptr<ChunkTransferContext> TransferManager::get_transfer_context(
    const std::string& chunk_id) const {
    std::lock_guard<std::mutex> lock(impl_->transfers_mutex);
    auto it = impl_->active_transfers.find(chunk_id);
    if (it != impl_->active_transfers.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace eliop2p
