#ifndef ELIOP2P_PROXY_SERVER_H
#define ELIOP2P_PROXY_SERVER_H

#include "eliop2p/base/config.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/p2p/transfer.h"
#include <string>
#include <memory>
#include <functional>
#include <cstdint>
#include <atomic>
#include <mutex>

namespace eliop2p {

struct ProxyMetrics {
    uint64_t total_requests = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t p2p_downloads = 0;
    uint64_t storage_downloads = 0;
    uint64_t total_bytes_served = 0;
    uint64_t total_bytes_downloaded = 0;
    double avg_latency_ms = 0.0;

    float cache_hit_rate() const {
        uint64_t total = cache_hits + cache_misses;
        return total > 0 ? static_cast<float>(cache_hits) / total : 0.0f;
    }
};

class ProxyServer {
public:
    ProxyServer(const ProxyConfig& config,
                std::shared_ptr<ChunkManager> cache_manager,
                std::shared_ptr<StorageClient> storage_client);
    ~ProxyServer();

    // Start the proxy server
    bool start();

    // Stop the proxy server
    void stop();

    // Get metrics (thread-safe copy)
    ProxyMetrics get_metrics() const;

    // Check if running
    bool is_running() const;

    // Set transfer manager for P2P fallback
    void set_transfer_manager(std::shared_ptr<TransferManager> transfer_manager);

    // Set transfer manager from unique_ptr
    void set_transfer_manager(std::unique_ptr<TransferManager> transfer_manager);

    // Set scheduler for coroutines (for global scheduler integration)
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

    // Enable/disable P2P fallback
    void set_p2p_fallback_enabled(bool enabled);

    // Get stop event file descriptor for external signaling
    // Returns -1 if server is not running
    int get_stop_event_fd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_PROXY_SERVER_H
