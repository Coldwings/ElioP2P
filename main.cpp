#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <csignal>
#include <thread>
#include <chrono>

#include "eliop2p/base/logger.h"
#include "eliop2p/base/config.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/control/client.h"
#include "eliop2p/storage/s3_client.h"

using namespace eliop2p;

// Forward declaration for storage client
namespace eliop2p {
class StorageClient;
}

// Global flag for signal handling
static volatile std::sig_atomic_t g_running = 1;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = 0;
    }
}

class ElioP2PApplication {
public:
    ElioP2PApplication() = default;
    ~ElioP2PApplication() = default;

    bool initialize() {
        Logger::instance().info("Initializing ElioP2P...");

        // Load configuration
        auto& config = Config::instance().get();
        config.node.node_id = "node_" + std::to_string(getpid());
        config.cache.memory_cache_size_mb = 4096;
        config.cache.disk_cache_size_mb = 102400;
        config.cache.chunk_size_mb = 16;
        config.p2p.listen_port = 9000;
        config.proxy.listen_port = 8080;
        config.control_plane.endpoint = "localhost";
        config.control_plane.port = 8081;

        // Initialize components
        cache_manager_ = std::make_shared<ChunkManager>(config.cache);

        // Initialize disk cache if path is configured
        if (!config.cache.disk_cache_path.empty()) {
            cache_manager_->set_disk_cache_path(config.cache.disk_cache_path);
            if (cache_manager_->initialize_disk_cache()) {
                Logger::instance().info("Disk cache initialized at: " + config.cache.disk_cache_path);
            }
        }

        // Initialize storage client (S3/OSS)
        storage_client_ = StorageClientFactory::create(config.storage);
        if (!storage_client_) {
            Logger::instance().warning("Failed to create storage client");
        }

        node_discovery_ = std::make_unique<NodeDiscovery>(config.p2p);
        transfer_manager_ = std::make_unique<TransferManager>(config.p2p);

        // Create proxy server with cache manager and storage client
        proxy_server_ = std::make_unique<ProxyServer>(config.proxy, cache_manager_, storage_client_);

        control_client_ = std::make_unique<ControlPlaneClient>(config.control_plane);

        Logger::instance().info("ElioP2P initialized successfully");
        return true;
    }

    bool start() {
        Logger::instance().info("Starting ElioP2P...");

        // Start components
        if (!node_discovery_->start()) {
            Logger::instance().error("Failed to start node discovery");
            return false;
        }

        if (!transfer_manager_->start()) {
            Logger::instance().error("Failed to start transfer manager");
            return false;
        }

        // Connect transfer manager to proxy server for P2P fallback
        proxy_server_->set_transfer_manager(std::move(transfer_manager_));

        if (!proxy_server_->start()) {
            Logger::instance().error("Failed to start proxy server");
            return false;
        }

        Logger::instance().info("ElioP2P started successfully");
        Logger::instance().info("Proxy server listening on port " +
                                std::to_string(Config::instance().get().proxy.listen_port));
        return true;
    }

    void stop() {
        Logger::instance().info("Stopping ElioP2P...");

        if (control_client_) {
            control_client_->disconnect();
        }
        proxy_server_->stop();
        transfer_manager_->stop();
        node_discovery_->stop();

        Logger::instance().info("ElioP2P stopped");
    }

    void run() {
        while (g_running) {
            // Main loop - in production this would handle events
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

private:
    std::shared_ptr<ChunkManager> cache_manager_;
    std::shared_ptr<StorageClient> storage_client_;
    std::unique_ptr<NodeDiscovery> node_discovery_;
    std::unique_ptr<TransferManager> transfer_manager_;
    std::unique_ptr<ProxyServer> proxy_server_;
    std::unique_ptr<ControlPlaneClient> control_client_;
};

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "ElioP2P - Distributed P2P Cache Acceleration System" << std::endl;
    std::cout << "Version: 0.1.0" << std::endl;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --help, -h     Show this help message" << std::endl;
            std::cout << "  --version, -v  Show version" << std::endl;
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "ElioP2P version 0.1.0" << std::endl;
            return 0;
        }
    }

    try {
        ElioP2PApplication app;

        if (!app.initialize()) {
            std::cerr << "Failed to initialize application" << std::endl;
            return 1;
        }

        if (!app.start()) {
            std::cerr << "Failed to start application" << std::endl;
            return 1;
        }

        app.run();
        app.stop();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
