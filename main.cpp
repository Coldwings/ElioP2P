#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

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

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = 0;
    }
}

class ElioP2PApplication {
public:
    ElioP2PApplication() = default;
    ~ElioP2PApplication() {
        // Ensure stop is called if app is destroyed while running
        if (proxy_server_) {
            proxy_server_->stop();
        }
    }

    bool initialize(int argc, char* argv[]) {
        Logger::instance().info("Initializing ElioP2P...");

        // Load configuration from environment variables first
        Config::instance().load_from_env();

        // Parse command line arguments
        Config::instance().parse_command_line(argc, argv);

        // Get config (with defaults applied from load_from_env)
        auto& config = Config::instance().get();

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
        // Main loop - wait for stop signal
        // This loop checks g_running which is set by signal handler
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Signal received, initiate graceful shutdown
        stop();
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
    // Set up signal handlers for graceful shutdown
    // Note: Don't block signals globally as this interferes with elio::serve()
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "ElioP2P - Distributed P2P Cache Acceleration System" << std::endl;
    std::cout << "Version: 0.1.0" << std::endl;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --help, -h          Show this help message" << std::endl;
            std::cout << "  --version, -v       Show version" << std::endl;
            std::cout << "  --config, -c         Config file path" << std::endl;
            std::cout << std::endl;
            std::cout << "Environment Variables:" << std::endl;
            std::cout << "  ELIOP2P_NODE_ID              Node identifier" << std::endl;
            std::cout << "  ELIOP2P_BIND_ADDRESS        Bind address" << std::endl;
            std::cout << "  ELIOP2P_LOG_LEVEL           Log level (DEBUG, INFO, WARNING, ERROR)" << std::endl;
            std::cout << "  ELIOP2P_CACHE_MEMORY_SIZE    Memory cache size (MB)" << std::endl;
            std::cout << "  ELIOP2P_CACHE_DISK_SIZE     Disk cache size (MB)" << std::endl;
            std::cout << "  ELIOP2P_P2P_PORT           P2P listen port (default: 9000)" << std::endl;
            std::cout << "  ELIOP2P_PROXY_PORT          HTTP proxy port (default: 8080)" << std::endl;
            std::cout << "  ELIOP2P_CONTROL_PLANE       Control plane endpoint" << std::endl;
            std::cout << "  ELIOP2P_STORAGE_ENDPOINT    Object storage endpoint" << std::endl;
            std::cout << "  ELIOP2P_STORAGE_REGION     Object storage region" << std::endl;
            std::cout << std::endl;
            std::cout << "Example:" << std::endl;
            std::cout << "  ELIOP2P_STORAGE_ENDPOINT=http://s3.amazonaws.com \\" << std::endl;
            std::cout << "  ELIOP2P_PROXY_PORT=8080 \\" << std::endl;
            std::cout << "  " << argv[0] << std::endl;
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "ElioP2P version 0.1.0" << std::endl;
            return 0;
        }
    }

    try {
        ElioP2PApplication app;

        if (!app.initialize(argc, argv)) {
            std::cerr << "Failed to initialize application" << std::endl;
            return 1;
        }

        if (!app.start()) {
            std::cerr << "Failed to start application" << std::endl;
            return 1;
        }

        app.run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
