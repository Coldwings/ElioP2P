#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <csignal>
#include <thread>
#include <chrono>
#include <atomic>

#include "CLI/CLI.hpp"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/config.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/control/client.h"
#include "eliop2p/control/server.h"
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
        if (control_plane_server_) {
            control_plane_server_->stop();
        }
    }

    bool initialize(int argc, char* argv[]) {
        Logger::instance().info("Initializing ElioP2P...");

        // Load configuration from environment variables first
        Config::instance().load_from_env();

        // Parse command line arguments
        // This may return false for --help, --version, or parse errors
        if (!Config::instance().parse_command_line(argc, argv)) {
            // parse_command_line returns false for --help/--version or errors
            // The error message should have already been printed
            return false;
        }

        // Get config (with defaults applied from load_from_env)
        auto& config = Config::instance().get();

        // Get server mode from config (set by CLI11 parsing)
        bool server_mode = Config::instance().is_server_mode();

        // Initialize control plane server if in server mode
        if (server_mode) {
            Logger::instance().info("Running in control plane server mode");
            control_plane_server_ = std::make_unique<ControlPlaneServer>(config.control_plane_server);
        } else {
            // Initialize components for regular node mode
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
        }

        Logger::instance().info("ElioP2P initialized successfully");
        return true;
    }

    bool start(bool server_mode = false) {
        Logger::instance().info("Starting ElioP2P...");

        if (server_mode) {
            // Start control plane server
            if (!control_plane_server_->start()) {
                Logger::instance().error("Failed to start control plane server");
                return false;
            }
            Logger::instance().info("Control plane server listening on port " +
                                    std::to_string(Config::instance().get().control_plane_server.listen_port));
            return true;
        }

        // Start components for regular node mode
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

    void stop(bool server_mode = false) {
        Logger::instance().info("Stopping ElioP2P...");

        if (server_mode) {
            if (control_plane_server_) {
                control_plane_server_->stop();
            }
        } else {
            if (control_client_) {
                control_client_->disconnect();
            }
            if (proxy_server_) {
                proxy_server_->stop();
            }
            if (transfer_manager_) {
                transfer_manager_->stop();
            }
            if (node_discovery_) {
                node_discovery_->stop();
            }
        }

        Logger::instance().info("ElioP2P stopped");
    }

    void run(bool server_mode = false) {
        // Main loop - wait for stop signal
        // This loop checks g_running which is set by signal handler
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        Logger::instance().info("About to call stop()");
        // Signal received, initiate graceful shutdown
        stop(server_mode);
        Logger::instance().info("stop() returned");

        // Use _exit to bypass normal cleanup which may cause segfault
        std::_Exit(0);
    }

private:
    std::shared_ptr<ChunkManager> cache_manager_;
    std::shared_ptr<StorageClient> storage_client_;
    std::unique_ptr<NodeDiscovery> node_discovery_;
    std::unique_ptr<TransferManager> transfer_manager_;
    std::unique_ptr<ProxyServer> proxy_server_;
    std::unique_ptr<ControlPlaneClient> control_client_;
    std::unique_ptr<ControlPlaneServer> control_plane_server_;
};

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful shutdown
    // Note: Don't block signals globally as this interferes with elio::serve()
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "ElioP2P - Distributed P2P Cache Acceleration System" << std::endl;
    std::cout << "Version: 0.1.0" << std::endl;

    try {
        ElioP2PApplication app;

        // Parse command line - CLI11 handles --help/--version automatically
        // parse_command_line returns false for --help/--version or errors
        if (!app.initialize(argc, argv)) {
            // Help/version messages already printed by CLI11
            return 0;
        }

        // Get server mode from config (set by CLI11)
        bool server_mode = Config::instance().is_server_mode();

        if (!app.start(server_mode)) {
            std::cerr << "Failed to start application" << std::endl;
            return 1;
        }

        app.run(server_mode);

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
