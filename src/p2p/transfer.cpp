#include "eliop2p/p2p/transfer.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/config.h"
#include <elio/net/tcp.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>
#include <elio/hash/sha256.hpp>
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
    uint64_t wait_time_ms = 0;

    // Use spinlock for short critical sections
    // Note: Must release lock before co_await to avoid blocking other coroutines
    {
        elio::sync::spinlock_guard lock(mutex_);

        // 0 means unlimited (matches P2PConfig convention)
        if (max_bytes_per_sec_ == 0) {
            co_return;
        }

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

        // Calculate wait time while holding lock, then release.
        // wait_time is a coroutine-local value: concurrent acquirers must not
        // share it (it used to be a member and got clobbered).
        wait_time_ms = ((bytes - available_) * 1000) / std::max<uint64_t>(max_bytes_per_sec_ / 1024, 1);
        available_ = 0;
    } // Lock released here

    // Wait outside the lock to not block other coroutines
    co_await elio::time::sleep_for(std::chrono::milliseconds(wait_time_ms));
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

// ---------------------------------------------------------------------------
// Wire protocol helpers
//
// ChunkMessageHeader is a packed struct placed directly on the wire. All
// multi-byte integer fields are converted to/from network byte order at the
// boundary so mixed-endianness peers interoperate.
// ---------------------------------------------------------------------------

static ChunkMessageHeader header_to_wire(ChunkMessageHeader h) {
    h.magic = htonl(h.magic);
    h.version = htonl(h.version);
    h.message_type = htonl(h.message_type);
    h.chunk_id_length = htonl(h.chunk_id_length);
    h.data_length = htonl(h.data_length);
    h.sequence_number = htonl(h.sequence_number);
    // hash[32] and flags are byte arrays, no conversion needed
    return h;
}

static ChunkMessageHeader header_from_wire(ChunkMessageHeader h) {
    h.magic = ntohl(h.magic);
    h.version = ntohl(h.version);
    h.message_type = ntohl(h.message_type);
    h.chunk_id_length = ntohl(h.chunk_id_length);
    h.data_length = ntohl(h.data_length);
    h.sequence_number = ntohl(h.sequence_number);
    return h;
}

// Read exactly n bytes (TCP may deliver short reads). Returns bytes actually
// read; anything < n means EOF or error before the buffer was filled.
static elio::coro::task<ssize_t> read_exact(elio::net::tcp_stream& stream, void* buf, size_t n) {
    size_t total = 0;
    auto* p = static_cast<char*>(buf);
    while (total < n) {
        auto r = co_await stream.read(p + total, n - total);
        if (r.result <= 0) {
            break;
        }
        total += static_cast<size_t>(r.result);
    }
    co_return static_cast<ssize_t>(total);
}

// Write exactly n bytes (stream.write may accept short writes). Returns
// false on the first short/error write.
static elio::coro::task<bool> write_exact(elio::net::tcp_stream& stream, const void* buf, size_t n) {
    size_t total = 0;
    const auto* p = static_cast<const char*>(buf);
    while (total < n) {
        auto r = co_await stream.write(p + total, n - total);
        if (r.result <= 0) {
            co_return false;
        }
        total += static_cast<size_t>(r.result);
    }
    co_return true;
}

// Compute raw SHA256 digest (32 bytes) of a buffer
static void sha256_raw(const uint8_t* data, size_t size, uint8_t out[32]) {
    auto digest = elio::hash::sha256(data, size);
    std::memcpy(out, digest.data(), 32);
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
    bool scheduler_owned = false;  // true if we created the scheduler, false if provided externally
    std::atomic<bool> tcp_server_running{false};
    std::thread tcp_server_thread;
    std::optional<elio::net::tcp_listener> tcp_listener;
    uint16_t listen_port = 0;
    std::atomic<bool> server_stopped{true};

    // Chunk data provider callback
    TransferManager::ChunkDataProvider chunk_data_provider;

    // Chunk data consumer callback (stores chunks received via Upload)
    TransferManager::ChunkDataConsumer chunk_data_consumer;

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
            // Read request header (loop: TCP delivers short reads)
            ChunkMessageHeader header;
            if (co_await read_exact(stream, &header, sizeof(header)) != static_cast<ssize_t>(sizeof(header))) {
                Logger::instance().warning("Invalid chunk request header size");
                co_return;
            }
            header = header_from_wire(header);

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
            if (co_await read_exact(stream, chunk_id_buf.data(), header.chunk_id_length) !=
                static_cast<ssize_t>(header.chunk_id_length)) {
                Logger::instance().warning("Failed to read chunk ID");
                co_return;
            }

            std::string chunk_id(chunk_id_buf.begin(), chunk_id_buf.end());
            Logger::instance().debug("Chunk request for: " + chunk_id);

            // Upload: the peer is pushing chunk data to us; store it via the
            // registered consumer and acknowledge with the received SHA256.
            if (header.message_type == static_cast<uint32_t>(ChunkMessageType::Upload)) {
                constexpr uint32_t MAX_UPLOAD_BYTES = 64 * 1024 * 1024;  // 4x CHUNK_SIZE
                if (header.data_length == 0 || header.data_length > MAX_UPLOAD_BYTES) {
                    Logger::instance().warning("Invalid upload data length: " +
                                               std::to_string(header.data_length));
                    co_return;
                }

                std::vector<uint8_t> data(header.data_length);
                if (co_await read_exact(stream, data.data(), data.size()) !=
                    static_cast<ssize_t>(data.size())) {
                    Logger::instance().warning("Failed to read upload data for: " + chunk_id);
                    co_return;
                }

                // Verify payload integrity before storing: the sender puts
                // SHA256(data) in the header. (This detects corruption; the
                // trust model for accepting peer content at all is enforced
                // at the proxy layer via origin-anchored meta hashes.)
                uint8_t computed[32];
                sha256_raw(data.data(), data.size(), computed);
                const bool hash_ok = (std::memcmp(computed, header.hash, 32) == 0);

                bool accepted = false;
                if (hash_ok && chunk_data_consumer) {
                    accepted = chunk_data_consumer(chunk_id, data);
                }

                ChunkMessageHeader ack{};
                ack.magic = CHUNK_TRANSFER_MAGIC;
                ack.version = CHUNK_TRANSFER_VERSION;
                ack.message_type = static_cast<uint32_t>(
                    accepted ? ChunkMessageType::Ack : ChunkMessageType::Error);
                ack.chunk_id_length = header.chunk_id_length;
                ack.data_length = 0;
                ack.flags = 0;
                if (accepted) {
                    sha256_raw(data.data(), data.size(), ack.hash);
                }

                auto ack_wire = header_to_wire(ack);
                if (co_await write_exact(stream, &ack_wire, sizeof(ack_wire))) {
                    co_await write_exact(stream, chunk_id.data(), chunk_id.size());
                }
                co_await stream.close();
                Logger::instance().info(std::string(accepted ? "Stored uploaded chunk: " :
                                                               "Rejected uploaded chunk: ") +
                                        chunk_id + " (" + std::to_string(data.size()) + " bytes)");
                co_return;
            }

            // Check if we have this chunk
            std::shared_ptr<const std::vector<uint8_t>> chunk_data;
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

                auto wire = header_to_wire(resp_header);
                if (co_await write_exact(stream, &wire, sizeof(wire))) {
                    co_await write_exact(stream, chunk_id_buf.data(), header.chunk_id_length);
                }
                Logger::instance().warning("Chunk not found: " + chunk_id);
                co_await stream.close();
                co_return;
            }

            // Send response with chunk data, integrity hash included so the
            // receiver can detect corruption (was a placeholder before).
            ChunkMessageHeader resp_header{};
            resp_header.magic = CHUNK_TRANSFER_MAGIC;
            resp_header.version = CHUNK_TRANSFER_VERSION;
            resp_header.message_type = static_cast<uint32_t>(ChunkMessageType::Response);
            resp_header.chunk_id_length = header.chunk_id_length;
            resp_header.data_length = static_cast<uint32_t>(chunk_data->size());
            resp_header.sequence_number = 0;
            resp_header.flags = ChunkMessageHeader::FLAG_LAST_PART;
            sha256_raw(chunk_data->data(), chunk_data->size(), resp_header.hash);

            auto wire = header_to_wire(resp_header);
            if (!co_await write_exact(stream, &wire, sizeof(wire))) {
                Logger::instance().error("Failed to send response header");
                co_await stream.close();
                co_return;
            }
            if (!co_await write_exact(stream, chunk_id_buf.data(), header.chunk_id_length)) {
                Logger::instance().error("Failed to send chunk_id");
                co_await stream.close();
                co_return;
            }

            // Send data in slices so upload bandwidth limiting actually
            // applies mid-stream instead of only once per chunk
            constexpr size_t SLICE = 256 * 1024;
            size_t sent = 0;
            bool send_ok = true;
            while (sent < chunk_data->size()) {
                size_t slice = std::min(SLICE, chunk_data->size() - sent);
                if (upload_limiter) {
                    co_await upload_limiter->acquire(slice);
                }
                if (!co_await write_exact(stream, chunk_data->data() + sent, slice)) {
                    send_ok = false;
                    break;
                }
                sent += slice;
            }
            if (!send_ok) {
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
                    if (!tcp_server_running) break;
                    continue;
                }

                // Handle request in background
                auto handler = handle_chunk_request(std::move(*stream_result));
                scheduler->spawn(elio::coro::detail::task_access::release(std::move(handler)));

            } catch (const std::exception& e) {
                if (tcp_server_running) {
                    Logger::instance().error("TCP server error: " + std::string(e.what()));
                } else {
                    break;
                }
            }
        }

        // Signal stop_tcp_server() that the loop actually exited
        server_stopped = true;
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

    // Start the chunk transfer TCP server so peers can download from us.
    // start_tcp_server is a lazy coroutine - spawn it or it never runs.
    if (!impl_->scheduler) {
        impl_->scheduler = std::make_shared<elio::runtime::scheduler>(2);
        impl_->scheduler_owned = true;
        impl_->scheduler->start();
    }
    auto server_task = start_tcp_server();
    impl_->scheduler->spawn(elio::coro::detail::task_access::release(std::move(server_task)));

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
        impl_->scheduler_owned = true;
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
    auto server_loop = impl_->tcp_server_loop();
    impl_->scheduler->spawn(elio::coro::detail::task_access::release(std::move(server_loop)));

    co_return;
}

void TransferManager::stop_tcp_server() {
    if (!impl_->tcp_server_running) {
        return;
    }

    Logger::instance().info("Stopping TCP chunk server...");
    impl_->tcp_server_running = false;

    // Self-connect wakeup: closing the listener fd does not necessarily
    // complete an io_uring accept pending on it. One dummy connection makes
    // the accept loop wake up and observe tcp_server_running == false.
    {
        int wake_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (wake_fd >= 0) {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = htons(impl_->listen_port);
            inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
            ::connect(wake_fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            ::close(wake_fd);
        }
    }

    if (impl_->tcp_listener) {
        impl_->tcp_listener->close();
        impl_->tcp_listener = std::nullopt;
    }

    // Wait for the accept loop to exit. If the listener close does not
    // interrupt a pending accept on this platform, give up after 2s and let
    // scheduler shutdown reclaim the coroutine instead of hanging forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!impl_->server_stopped.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!impl_->server_stopped.load()) {
        Logger::instance().warning("TCP chunk server stop timed out (accept still pending)");
    }

    Logger::instance().info("TCP chunk server stopped");
}

void TransferManager::set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) {
    impl_->scheduler = std::move(scheduler);
    impl_->scheduler_owned = false;  // Scheduler is now externally owned
}

void TransferManager::set_chunk_data_provider(ChunkDataProvider provider) {
    impl_->chunk_data_provider = std::move(provider);
}

void TransferManager::set_chunk_data_consumer(ChunkDataConsumer consumer) {
    impl_->chunk_data_consumer = std::move(consumer);
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

// Result of a single-peer download attempt
struct PeerDownloadResult {
    bool success = false;
    uint64_t bytes_downloaded = 0;
    std::vector<uint8_t> data;
};

// Download the full chunk from a single peer into a PRIVATE buffer.
// Multiple of these race each other in hedged mode: the caller adopts the
// first verified result and signals ctx->stop_flag to abort the rest.
elio::coro::task<PeerDownloadResult> download_from_peer(
    BandwidthLimiter* download_limiter,
    std::shared_ptr<ChunkTransferContext> ctx,
    PeerNode peer,
    TransferProgressCallback progress_callback,
    std::string expected_sha256 = "") {

    PeerDownloadResult result;
    const uint64_t slice_size = 256 * 1024;

    try {
        Logger::instance().debug("Starting TCP download from peer: " + peer.node_id + " at " + peer.address + ":" + std::to_string(peer.port));

        // Establish TCP connection to peer
        elio::net::tcp_options opts;
        opts.no_delay = true;

        auto connect_result = co_await elio::net::tcp_connect(
            elio::net::socket_address(peer.address, peer.port), opts);
        if (!connect_result) {
            Logger::instance().error("Failed to connect to peer: " + peer.node_id);
            co_return result;
        }

        elio::net::tcp_stream& stream = *connect_result;

        // Build and send request
        const std::string& chunk_id = ctx->chunk_id;

        ChunkMessageHeader header{};
        header.magic = CHUNK_TRANSFER_MAGIC;
        header.version = CHUNK_TRANSFER_VERSION;
        header.message_type = static_cast<uint32_t>(ChunkMessageType::Request);
        header.chunk_id_length = static_cast<uint32_t>(chunk_id.size());
        header.data_length = 0;
        header.sequence_number = 0;
        header.flags = ctx->is_resume ? ChunkMessageHeader::FLAG_RESUME : 0;

        auto wire = header_to_wire(header);
        if (!co_await write_exact(stream, &wire, sizeof(wire))) {
            Logger::instance().error("Failed to send request header to peer: " + peer.node_id);
            co_await stream.close();
            co_return result;
        }

        if (!co_await write_exact(stream, chunk_id.data(), chunk_id.size())) {
            Logger::instance().error("Failed to send chunk_id to peer: " + peer.node_id);
            co_await stream.close();
            co_return result;
        }

        // Read response header
        ChunkMessageHeader resp_header;
        if (co_await read_exact(stream, &resp_header, sizeof(resp_header)) !=
            static_cast<ssize_t>(sizeof(resp_header))) {
            Logger::instance().error("Failed to read response header from peer: " + peer.node_id);
            co_await stream.close();
            co_return result;
        }
        resp_header = header_from_wire(resp_header);

        if (resp_header.magic != CHUNK_TRANSFER_MAGIC) {
            Logger::instance().error("Invalid magic in response from peer: " + peer.node_id);
            co_await stream.close();
            co_return result;
        }

        if (resp_header.message_type == static_cast<uint32_t>(ChunkMessageType::Error)) {
            Logger::instance().error("Peer returned error for chunk: " + ctx->chunk_id);
            co_await stream.close();
            co_return result;
        }

        if (resp_header.message_type != static_cast<uint32_t>(ChunkMessageType::Response)) {
            Logger::instance().error("Unexpected message type from peer: " + peer.node_id);
            co_await stream.close();
            co_return result;
        }

        // The server echoes the chunk_id after the response header; consume
        // it before the payload or every byte would be misaligned.
        if (resp_header.chunk_id_length > 0 && resp_header.chunk_id_length <= 256) {
            std::vector<char> id_buf(resp_header.chunk_id_length);
            if (co_await read_exact(stream, id_buf.data(), id_buf.size()) !=
                static_cast<ssize_t>(id_buf.size())) {
                Logger::instance().error("Failed to read echoed chunk_id from peer: " + peer.node_id);
                co_await stream.close();
                co_return result;
            }
        }

        // Read chunk data into the private buffer
        const uint64_t total_data_length = resp_header.data_length;
        result.data.reserve(total_data_length);

        TransferProgress progress;
        progress.chunk_id = ctx->chunk_id;
        progress.total_bytes = ctx->total_size;
        progress.current_peer = peer.node_id;

        while (result.bytes_downloaded < total_data_length) {
            // Hedged mode: another peer already won, or explicit cancel
            if (ctx->stop_flag->load(std::memory_order_relaxed)) {
                Logger::instance().info("Download from peer " + peer.node_id + " aborted (race lost or cancelled)");
                co_await stream.close();
                result.data.clear();
                result.data.shrink_to_fit();
                co_return result;
            }

            uint64_t read_size = std::min(slice_size, total_data_length - result.bytes_downloaded);

            if (download_limiter) {
                co_await download_limiter->acquire(read_size);
            }

            std::vector<uint8_t> slice(read_size);
            auto got = co_await read_exact(stream, slice.data(), read_size);
            if (got <= 0) {
                Logger::instance().error("Connection closed while reading data from peer: " + peer.node_id);
                co_await stream.close();
                result.data.clear();
                co_return result;
            }

            slice.resize(static_cast<size_t>(got));
            result.data.insert(result.data.end(), slice.begin(), slice.end());
            result.bytes_downloaded += static_cast<uint64_t>(got);

            // Per-peer progress (no shared counters - races otherwise)
            progress.bytes_transferred = result.bytes_downloaded;
            progress.progress_percent = ctx->total_size > 0
                ? static_cast<double>(result.bytes_downloaded) / ctx->total_size * 100.0
                : 0.0;
            if (progress_callback) {
                progress_callback(progress);
            }
        }

        // Close socket
        co_await stream.close();

        // Integrity check: SHA256 over the received data must match the
        // hash the sender put in the response header
        uint8_t computed[32];
        sha256_raw(result.data.data(), result.data.size(), computed);
        if (std::memcmp(computed, resp_header.hash, 32) != 0) {
            Logger::instance().error("SHA256 mismatch for chunk " + ctx->chunk_id +
                                     " from peer " + peer.node_id + ", discarding");
            result.data.clear();
            result.bytes_downloaded = 0;
            co_return result;
        }

        // Origin-anchored content check: when the caller knows the true
        // hash (learned from the storage origin), enforce it so a peer
        // serving wrong bytes with a consistent self-hash still loses.
        if (!expected_sha256.empty()) {
            auto digest = elio::hash::sha256(result.data.data(), result.data.size());
            const std::string hex = elio::hash::sha256_hex(digest);
            if (hex != expected_sha256) {
                Logger::instance().error("Origin-anchored hash mismatch for chunk " +
                                         ctx->chunk_id + " from peer " + peer.node_id +
                                         " (expected " + expected_sha256.substr(0, 16) + "...)");
                result.data.clear();
                result.bytes_downloaded = 0;
                co_return result;
            }
        }

        Logger::instance().info("Successfully downloaded " + std::to_string(result.bytes_downloaded) +
                               " bytes from peer: " + peer.node_id);
        result.success = true;
        co_return result;

    } catch (const std::exception& e) {
        Logger::instance().error("Download failed from peer " + peer.node_id + ": " + e.what());
        result.data.clear();
        co_return result;
    } catch (...) {
        Logger::instance().error("Unknown error downloading from peer: " + peer.node_id);
        result.data.clear();
        co_return result;
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

    // Hedged parallel download: selected peers race on PRIVATE buffers.
    // The first peer returning a complete, SHA256-verified copy wins;
    // ctx->stop_flag then aborts the remaining downloads. This replaces the
    // old design where every peer wrote into one shared buffer (data race +
    // guaranteed corruption) and all K transfers always ran to completion.
    TransferProgress progress;
    progress.chunk_id = request.chunk_id;
    progress.total_bytes = ctx->total_size;
    for (const auto& peer : selected_peers) {
        progress.active_peers.push_back(peer.node_id);
    }

    std::vector<elio::coro::join_handle<PeerDownloadResult>> download_tasks;
    for (const auto& peer : selected_peers) {
        download_tasks.push_back(impl_->scheduler->go_joinable(
            download_from_peer,
            impl_->download_limiter.get(),
            ctx,
            peer,
            progress_callback,
            request.expected_sha256));
    }

    bool download_success = false;
    for (auto& task : download_tasks) {
        auto r = co_await task;
        if (download_success) {
            continue;  // race already decided; drain quietly
        }
        if (r.success) {
            // First verified winner: adopt its data and abort the others
            ctx->stop_flag->store(true, std::memory_order_relaxed);
            ctx->downloaded_size = r.bytes_downloaded;
            result = std::move(r.data);
            download_success = true;
            Logger::instance().info("Hedged download won by a peer, " +
                                    std::to_string(r.bytes_downloaded) + " bytes verified");
        }
    }

    // If no peer succeeded, try fallback peers one by one
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

        for (const auto& peer : fallback_peers) {
            if (!impl_->running || ctx->stop_flag->load(std::memory_order_relaxed)) {
                break;
            }

            Logger::instance().info("Trying fallback peer: " + peer.node_id);

            try {
                auto r = co_await download_from_peer(
                    impl_->download_limiter.get(),
                    ctx,
                    peer,
                    progress_callback,
                    request.expected_sha256);

                if (r.success) {
                    ctx->stop_flag->store(true, std::memory_order_relaxed);
                    ctx->downloaded_size = r.bytes_downloaded;
                    result = std::move(r.data);
                    download_success = true;
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
        impl_->remove_transfer(request.chunk_id);
        co_return std::nullopt;
    }

    // Final checkpoint
    save_progress(request.chunk_id, ctx->downloaded_size.load());

    // Clean up progress file on success
    std::filesystem::remove(ctx->file_path);
    impl_->remove_transfer(request.chunk_id);

    progress.completed = true;
    progress.bytes_transferred = ctx->downloaded_size.load();
    progress.progress_percent = 100.0;

    if (progress_callback) {
        progress_callback(progress);
    }

    impl_->stats.total_bytes_downloaded += result.size();

    Logger::instance().info("Download completed for chunk: " + request.chunk_id +
                            ", size: " + std::to_string(result.size()) + " bytes");

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

    // Reuse the hedged download_chunk (racing peers, private buffers, SHA256
    // verification), then persist the winning copy. Chunks are bounded by
    // CHUNK_SIZE (16MB) so holding one in memory is fine.
    auto data = co_await download_chunk(request, progress_callback);
    if (!data) {
        co_return false;
    }

    std::ofstream out_file(dest_path, std::ios::out | std::ios::binary);
    if (!out_file.is_open()) {
        Logger::instance().error("Failed to open file for writing: " + dest_path);
        co_return false;
    }
    out_file.write(reinterpret_cast<const char*>(data->data()),
                   static_cast<std::streamsize>(data->size()));
    out_file.flush();
    if (!out_file) {
        Logger::instance().error("Failed to write chunk data to: " + dest_path);
        co_return false;
    }
    out_file.close();

    Logger::instance().info("File download completed for chunk: " + request.chunk_id +
                            ", size: " + std::to_string(data->size()) + " bytes");
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

        auto connect_result = co_await elio::net::tcp_connect(
            elio::net::socket_address(target_peer.address, target_peer.port), opts);
        if (!connect_result) {
            Logger::instance().error("Failed to connect to peer for upload: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_return false;
        }

        elio::net::tcp_stream& stream = *connect_result;

        // Build Upload header (wire format on send). The hash lets the
        // receiver verify payload integrity before storing.
        ChunkMessageHeader header{};
        header.magic = CHUNK_TRANSFER_MAGIC;
        header.version = CHUNK_TRANSFER_VERSION;
        header.message_type = static_cast<uint32_t>(ChunkMessageType::Upload);
        header.chunk_id_length = static_cast<uint32_t>(chunk_id.size());
        header.data_length = static_cast<uint32_t>(data.size());
        header.sequence_number = 0;
        header.flags = ChunkMessageHeader::FLAG_LAST_PART;
        sha256_raw(data.data(), data.size(), header.hash);

        auto wire = header_to_wire(header);
        if (!co_await write_exact(stream, &wire, sizeof(wire))) {
            Logger::instance().error("Failed to send upload request header to peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        if (!co_await write_exact(stream, chunk_id.data(), chunk_id.size())) {
            Logger::instance().error("Failed to send chunk_id to peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // Upload data in slices with bandwidth limiting
        const uint64_t slice_size = 256 * 1024;
        uint64_t offset = 0;

        while (offset < data.size()) {
            uint64_t bytes_this_slice = std::min(slice_size, static_cast<uint64_t>(data.size()) - offset);
            if (impl_->upload_limiter) {
                co_await impl_->upload_limiter->acquire(bytes_this_slice);
            }

            if (!co_await write_exact(stream, data.data() + offset, bytes_this_slice)) {
                Logger::instance().error("Connection closed while uploading to peer: " + target_peer.node_id);
                impl_->stats.failed_uploads++;
                co_await stream.close();
                co_return false;
            }
            offset += bytes_this_slice;
        }

        // Read Ack/Error response
        ChunkMessageHeader resp_header;
        if (co_await read_exact(stream, &resp_header, sizeof(resp_header)) !=
            static_cast<ssize_t>(sizeof(resp_header))) {
            Logger::instance().error("Failed to read upload response from peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }
        resp_header = header_from_wire(resp_header);

        // The Ack carries the chunk_id again; consume it if present
        if (resp_header.chunk_id_length > 0 && resp_header.chunk_id_length <= 256) {
            std::vector<char> id_buf(resp_header.chunk_id_length);
            co_await read_exact(stream, id_buf.data(), id_buf.size());
        }

        // Validate response
        if (resp_header.magic != CHUNK_TRANSFER_MAGIC) {
            Logger::instance().error("Invalid magic in upload response from peer: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        if (resp_header.message_type != static_cast<uint32_t>(ChunkMessageType::Ack)) {
            Logger::instance().error("Peer rejected upload: " + target_peer.node_id);
            impl_->stats.failed_uploads++;
            co_await stream.close();
            co_return false;
        }

        // The Ack echoes the receiver's SHA256 of what it stored; it must
        // match our copy or the transfer was corrupted
        uint8_t computed[32];
        sha256_raw(data.data(), data.size(), computed);
        if (std::memcmp(computed, resp_header.hash, 32) != 0) {
            Logger::instance().error("Upload hash mismatch reported by peer: " + target_peer.node_id);
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
        // Signal all in-flight peer downloads for this chunk to abort.
        // The download loops poll this flag between slices.
        it->second->stop_flag->store(true, std::memory_order_relaxed);
        // Save progress before cancellation for resume
        save_progress(chunk_id, it->second->downloaded_size.load());
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

void TransferManager::announce_local_chunk(const std::string& chunk_id) {
    if (impl_->node_discovery) {
        impl_->node_discovery->announce_chunk(chunk_id);
    }
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
