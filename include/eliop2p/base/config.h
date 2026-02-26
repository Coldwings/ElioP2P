#ifndef ELIOP2P_BASE_CONFIG_H
#define ELIOP2P_BASE_CONFIG_H

#include <string>
#include <optional>
#include <cstdint>
#include <vector>
#include <map>

namespace eliop2p {

// Log configuration
struct LogConfig {
    std::string level = "info";
    std::string output = "stdout";  // stdout, stderr, file
    std::string file_path = "";
    size_t max_file_size_mb = 100;
    size_t max_backup_files = 5;
};

// Node configuration
struct NodeConfig {
    std::string node_id;
    std::string bind_address = "0.0.0.0";
    uint16_t http_port = 0;  // 0 means random port
};

// Cache configuration
struct CacheConfig {
    uint64_t memory_cache_size_mb = 4096;
    uint64_t disk_cache_size_mb = 102400;
    uint32_t chunk_size_mb = 16;
    float eviction_threshold = 0.8;
    float eviction_target = 0.6;
    std::string disk_cache_path = "/var/cache/eliop2p";
    uint32_t eviction_protection_sec = 60;

    // Multi-factor weighted LRU eviction weights
    float eviction_weight_time = 1.0f;    // w1: time since last access
    float eviction_weight_replica = 0.5f; // w2: replica count
    float eviction_weight_heat = 0.3f;    // w3: heat level

    // Heat level thresholds (accesses per hour)
    uint32_t hot_threshold = 100;   // >100/h: hot
    uint32_t warm_threshold = 10;   // 10-100/h: warm
    // <10/h: cold
};

// P2P configuration
struct P2PConfig {
    uint16_t listen_port = 9000;
    uint32_t max_connections = 100;
    uint32_t max_peers = 50;
    uint64_t max_upload_speed_mbps = 0;  // 0 = unlimited
    uint64_t max_download_speed_mbps = 0;  // 0 = unlimited
    uint32_t selection_k = 5;
    uint32_t gossip_interval_sec = 10;
    uint32_t heartbeat_timeout_sec = 60;
    std::string transport = "tcp";  // tcp, rdma
};

// Control plane configuration
struct ControlPlaneConfig {
    std::string endpoint;
    uint16_t port = 8081;
    uint32_t heartbeat_interval_sec = 30;
    uint32_t reconnect_interval_sec = 5;
    bool enable = true;
};

// Control plane server configuration (when running as server)
struct ControlPlaneServerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t listen_port = 8082;
    uint32_t heartbeat_timeout_sec = 90;
    uint32_t cleanup_interval_sec = 30;
    uint32_t max_nodes = 1000;
    bool enable_replica_adjustment = true;
    uint32_t min_replicas = 2;
    uint32_t max_replicas = 5;
};

// Storage configuration
struct StorageConfig {
    std::string type = "s3";  // s3, oss
    std::string endpoint;
    std::string region = "us-east-1";
    std::optional<std::string> access_key;
    std::optional<std::string> secret_key;
    std::string bucket;
    bool use_https = true;
    uint32_t connection_timeout_sec = 30;
    uint32_t read_timeout_sec = 60;
};

// Proxy configuration
struct ProxyConfig {
    uint16_t listen_port = 8080;
    std::string bind_address = "0.0.0.0";
    uint32_t max_connections = 1000;
    std::string auth_type = "none";  // none, basic, token
    std::optional<std::string> auth_token;
};

// Global configuration
struct GlobalConfig {
    LogConfig log;
    NodeConfig node;
    CacheConfig cache;
    P2PConfig p2p;
    ControlPlaneConfig control_plane;
    ControlPlaneServerConfig control_plane_server;
    StorageConfig storage;
    ProxyConfig proxy;
};

class Config {
public:
    static Config& instance();

    // Load configuration from file (YAML or JSON)
    bool load_from_file(const std::string& path);

    // Load configuration from environment variables
    bool load_from_env();

    // Parse command line arguments and override config
    bool parse_command_line(int argc, char* argv[]);

    // Get configuration
    const GlobalConfig& get() const { return config_; }
    GlobalConfig& get() { return config_; }

    // Get config file path that was loaded
    const std::string& get_config_file() const { return config_file_; }

    // Check if required fields are set
    bool validate() const;

    // Print configuration (for debugging)
    void print() const;

private:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    bool load_yaml(const std::string& path);
    bool load_json(const std::string& path);
    void override_from_env();
    void override_from_cmdline(int argc, char* argv[]);

    GlobalConfig config_;
    std::string config_file_;
};

} // namespace eliop2p

#endif // ELIOP2P_BASE_CONFIG_H
