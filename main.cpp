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

    bool initialize(int argc, char* argv[], bool server_mode = false) {
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

        // Store server mode
        server_mode_ = server_mode;

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
    bool server_mode_ = false;
};

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful shutdown
    // Note: Don't block signals globally as this interferes with elio::serve()
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "ElioP2P - Distributed P2P Cache Acceleration System" << std::endl;
    std::cout << "Version: 0.1.0" << std::endl;

    // Check for help/version flags before full initialization
    // CLI11 handles --help and --version automatically when parsing
    // But we need to detect them early to display the full help with environment variables
    bool show_help = false;
    bool show_version = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg == "--version" || arg == "-v") {
            show_version = true;
        }
    }

    if (show_help) {
        std::cout << std::endl;
        std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
        std::cout << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -c, --config <path>       Config file path" << std::endl;
        std::cout << "  -v, --version             Show version" << std::endl;
        std::cout << "  -h, --help                Show this help message" << std::endl;
        std::cout << "  -s, --server              Run as control plane server mode" << std::endl;
        std::cout << std::endl;
        std::cout << "Node Options:" << std::endl;
        std::cout << "  --node-id <id>            Node identifier" << std::endl;
        std::cout << "  --bind-address <addr>     Bind address (default: 0.0.0.0)" << std::endl;
        std::cout << "  --http-port <port>        HTTP listen port" << std::endl;
        std::cout << std::endl;
        std::cout << "Log Options:" << std::endl;
        std::cout << "  --log-level <level>       Log level (debug, info, warning, error)" << std::endl;
        std::cout << "  --log-output <output>     Log output (stdout, stderr, file)" << std::endl;
        std::cout << "  --log-file <path>         Log file path" << std::endl;
        std::cout << std::endl;
        std::cout << "Cache Options:" << std::endl;
        std::cout << "  --cache-memory-size <MB>   Memory cache size in MB" << std::endl;
        std::cout << "  --cache-disk-size <MB>    Disk cache size in MB" << std::endl;
        std::cout << "  --cache-chunk-size <MB>   Chunk size in MB" << std::endl;
        std::cout << "  --cache-disk-path <path>  Disk cache path" << std::endl;
        std::cout << "  --cache-eviction-threshold <val>  Eviction threshold (0.0-1.0)" << std::endl;
        std::cout << "  --cache-eviction-target <val>      Eviction target (0.0-1.0)" << std::endl;
        std::cout << std::endl;
        std::cout << "P2P Options:" << std::endl;
        std::cout << "  --p2p-port <port>         P2P listen port (default: 9000)" << std::endl;
        std::cout << "  --p2p-max-connections <n> Maximum P2P connections" << std::endl;
        std::cout << "  --p2p-max-peers <n>       Maximum number of peers" << std::endl;
        std::cout << "  --p2p-max-upload-speed <Mbps>  Max upload speed (0=unlimited)" << std::endl;
        std::cout << "  --p2p-max-download-speed <Mbps> Max download speed (0=unlimited)" << std::endl;
        std::cout << "  --p2p-selection-k <n>     Number of peers to select for transfer" << std::endl;
        std::cout << "  --p2p-gossip-interval <sec>  Gossip interval in seconds" << std::endl;
        std::cout << "  --p2p-heartbeat-timeout <sec>  Heartbeat timeout in seconds" << std::endl;
        std::cout << "  --p2p-transport <type>   Transport type (tcp, rdma)" << std::endl;
        std::cout << std::endl;
        std::cout << "Control Plane Options:" << std::endl;
        std::cout << "  --control-plane-endpoint <url>  Control plane endpoint" << std::endl;
        std::cout << "  --control-plane-port <port>     Control plane port" << std::endl;
        std::cout << "  --control-plane-heartbeat <sec> Heartbeat interval (seconds)" << std::endl;
        std::cout << "  --control-plane-reconnect <sec> Reconnect interval (seconds)" << std::endl;
        std::cout << "  --control-plane-enable          Enable control plane" << std::endl;
        std::cout << std::endl;
        std::cout << "Control Plane Server Options (--server mode):" << std::endl;
        std::cout << "  --control-server-port <port>    Control plane server listen port (default: 8082)" << std::endl;
        std::cout << "  --control-server-bind <addr>    Control plane server bind address" << std::endl;
        std::cout << "  --control-server-timeout <sec>  Heartbeat timeout for nodes" << std::endl;
        std::cout << "  --control-server-max-nodes <n>  Maximum registered nodes" << std::endl;
        std::cout << "  --control-server-min-replicas <n>  Minimum replica count per chunk" << std::endl;
        std::cout << "  --control-server-max-replicas <n>  Maximum replica count per chunk" << std::endl;
        std::cout << std::endl;
        std::cout << "Storage Options:" << std::endl;
        std::cout << "  --storage-type <type>      Storage type (s3, oss)" << std::endl;
        std::cout << "  --storage-endpoint <url>   Object storage endpoint URL" << std::endl;
        std::cout << "  --storage-region <region>  Object storage region" << std::endl;
        std::cout << "  --storage-bucket <bucket>  Object storage bucket" << std::endl;
        std::cout << "  --storage-access-key <key>  Object storage access key" << std::endl;
        std::cout << "  --storage-secret-key <key>  Object storage secret key" << std::endl;
        std::cout << "  --storage-https             Use HTTPS for storage" << std::endl;
        std::cout << "  --storage-connection-timeout <sec>  Connection timeout (seconds)" << std::endl;
        std::cout << "  --storage-read-timeout <sec>        Read timeout (seconds)" << std::endl;
        std::cout << std::endl;
        std::cout << "Proxy Options:" << std::endl;
        std::cout << "  --proxy-port <port>        HTTP proxy listen port (default: 8080)" << std::endl;
        std::cout << "  --proxy-bind-address <addr>  HTTP proxy bind address" << std::endl;
        std::cout << "  --proxy-max-connections <n> Maximum proxy connections" << std::endl;
        std::cout << "  --proxy-auth-type <type>  Authentication type (none, basic, token)" << std::endl;
        std::cout << "  --proxy-auth-token <token> Authentication token" << std::endl;
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
        std::cout << "  " << argv[0] << " --node-id=my-node" << std::endl;
        return 0;
    }

    if (show_version) {
        std::cout << "ElioP2P version 0.1.0" << std::endl;
        return 0;
    }

    try {
        ElioP2PApplication app;

        // Check for server mode flag
        bool server_mode = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--server" || arg == "-s") {
                server_mode = true;
                break;
            }
        }

        // Parse command line - this may return false for --help/--version or errors
        if (!app.initialize(argc, argv, server_mode)) {
            std::cerr << "Failed to initialize application" << std::endl;
            return 1;
        }

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
