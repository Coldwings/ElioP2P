#include "eliop2p/base/config.h"
#include "eliop2p/base/logger.h"
#include "CLI/CLI.hpp"
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace eliop2p {

namespace {

// Log level parsing - used when setting log level from config
static LogLevel parse_log_level(const std::string& level) {
    if (level == "debug") return elio::log::level::debug;
    if (level == "info") return elio::log::level::info;
    if (level == "warning") return elio::log::level::warning;
    if (level == "error") return elio::log::level::error;
    return elio::log::level::info;
}

std::string trim(const std::string& str) {
    auto start = std::find_if(str.begin(), str.end(), [](unsigned char c) { return !std::isspace(c); });
    auto end = std::find_if(str.rbegin(), str.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : "";
}

// Simple INI-style parser for config files
void parse_ini_file(const std::string& path, std::map<std::string, std::map<std::string, std::string>>& sections) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string current_section;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            sections[current_section] = {};
            continue;
        }

        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = trim(line.substr(0, pos));
            std::string value = trim(line.substr(pos + 1));
            // Remove quotes
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
                value = value.substr(1, value.size() - 2);
            }
            sections[current_section][key] = value;
        }
    }
}

} // anonymous namespace

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::load_from_file(const std::string& path) {
    Logger::instance().info("Loading config from file: " + path);

    if (!std::filesystem::exists(path)) {
        Logger::instance().warning("Config file not found: " + path);
        return false;
    }

    config_file_ = path;

    // Try to detect file format and parse
    std::string ext = std::filesystem::path(path).extension();
    if (ext == ".yaml" || ext == ".yml") {
        return load_yaml(path);
    } else if (ext == ".json") {
        return load_json(path);
    } else {
        // Default to INI format
        std::map<std::string, std::map<std::string, std::string>> sections;
        parse_ini_file(path, sections);

        // Parse log section
        if (sections.count("log")) {
            auto& s = sections["log"];
            if (s.count("level")) {
                config_.log.level = s["level"];
                Logger::instance().set_level(parse_log_level(s["level"]));
            }
            if (s.count("output")) config_.log.output = s["output"];
            if (s.count("file_path")) config_.log.file_path = s["file_path"];
        }

        // Parse node section
        if (sections.count("node")) {
            auto& s = sections["node"];
            if (s.count("node_id")) config_.node.node_id = s["node_id"];
            if (s.count("bind_address")) config_.node.bind_address = s["bind_address"];
            if (s.count("http_port")) config_.node.http_port = std::stoi(s["http_port"]);
        }

        // Parse cache section
        if (sections.count("cache")) {
            auto& s = sections["cache"];
            if (s.count("memory_cache_size_mb")) config_.cache.memory_cache_size_mb = std::stoull(s["memory_cache_size_mb"]);
            if (s.count("disk_cache_size_mb")) config_.cache.disk_cache_size_mb = std::stoull(s["disk_cache_size_mb"]);
            if (s.count("chunk_size_mb")) config_.cache.chunk_size_mb = std::stoi(s["chunk_size_mb"]);
            if (s.count("disk_cache_path")) config_.cache.disk_cache_path = s["disk_cache_path"];
        }

        // Parse p2p section
        if (sections.count("p2p")) {
            auto& s = sections["p2p"];
            if (s.count("listen_port")) config_.p2p.listen_port = std::stoi(s["listen_port"]);
            if (s.count("max_connections")) config_.p2p.max_connections = std::stoi(s["max_connections"]);
            if (s.count("max_peers")) config_.p2p.max_peers = std::stoi(s["max_peers"]);
            if (s.count("selection_k")) config_.p2p.selection_k = std::stoi(s["selection_k"]);
        }

        // Parse control_plane section
        if (sections.count("control_plane")) {
            auto& s = sections["control_plane"];
            if (s.count("endpoint")) config_.control_plane.endpoint = s["endpoint"];
            if (s.count("port")) config_.control_plane.port = std::stoi(s["port"]);
            if (s.count("enable")) config_.control_plane.enable = (s["enable"] == "true");
        }

        // Parse storage section
        if (sections.count("storage")) {
            auto& s = sections["storage"];
            if (s.count("type")) config_.storage.type = s["type"];
            if (s.count("endpoint")) config_.storage.endpoint = s["endpoint"];
            if (s.count("region")) config_.storage.region = s["region"];
            if (s.count("bucket")) config_.storage.bucket = s["bucket"];
            if (s.count("access_key")) config_.storage.access_key = s["access_key"];
            if (s.count("secret_key")) config_.storage.secret_key = s["secret_key"];
        }

        // Parse proxy section
        if (sections.count("proxy")) {
            auto& s = sections["proxy"];
            if (s.count("listen_port")) config_.proxy.listen_port = std::stoi(s["listen_port"]);
            if (s.count("bind_address")) config_.proxy.bind_address = s["bind_address"];
            if (s.count("max_connections")) config_.proxy.max_connections = std::stoi(s["max_connections"]);
        }

        Logger::instance().info("Config loaded successfully from: " + path);
        return true;
    }
}

bool Config::load_yaml(const std::string& path) {
    // Simplified YAML parsing - in production use a proper YAML library
    Logger::instance().warning("YAML parsing not fully implemented, using fallback");
    return load_from_file(path);  // Fall back to trying as INI
}

bool Config::load_json(const std::string& path) {
    // Simplified JSON parsing - in production use a proper JSON library
    Logger::instance().warning("JSON parsing not fully implemented, using fallback");
    return load_from_file(path);  // Fall back to trying as INI
}

bool Config::load_from_env() {
    Logger::instance().info("Loading config from environment variables");

    override_from_env();
    return true;
}

void Config::override_from_env() {
    // Node config
    if (const char* val = std::getenv("ELIOP2P_NODE_ID")) {
        config_.node.node_id = val;
    }
    if (const char* val = std::getenv("ELIOP2P_BIND_ADDRESS")) {
        config_.node.bind_address = val;
    }

    // Log config
    if (const char* val = std::getenv("ELIOP2P_LOG_LEVEL")) {
        config_.log.level = val;
    }

    // Control plane
    if (const char* val = std::getenv("ELIOP2P_CONTROL_PLANE")) {
        config_.control_plane.endpoint = val;
    }
    if (const char* val = std::getenv("ELIOP2P_CONTROL_PLANE_PORT")) {
        config_.control_plane.port = std::stoi(val);
    }

    // Storage
    if (const char* val = std::getenv("ELIOP2P_STORAGE_ENDPOINT")) {
        config_.storage.endpoint = val;
    }
    if (const char* val = std::getenv("ELIOP2P_STORAGE_REGION")) {
        config_.storage.region = val;
    }
    if (const char* val = std::getenv("ELIOP2P_STORAGE_BUCKET")) {
        config_.storage.bucket = val;
    }

    // P2P
    if (const char* val = std::getenv("ELIOP2P_P2P_PORT")) {
        config_.p2p.listen_port = std::stoi(val);
    }

    // Proxy
    if (const char* val = std::getenv("ELIOP2P_PROXY_PORT")) {
        config_.proxy.listen_port = std::stoi(val);
    }
}

bool Config::parse_command_line(int argc, char* argv[]) {
    // Create CLI11 app with description
    CLI::App app{"ElioP2P - Distributed P2P Cache Acceleration System"};

    // Config file option
    std::string config_file;
    app.add_option("-c,--config", config_file, "Path to configuration file");

    // Node options
    app.add_option("--node-id", config_.node.node_id, "Node identifier");
    app.add_option("--bind-address", config_.node.bind_address, "Bind address");
    app.add_option("--http-port", config_.node.http_port, "HTTP listen port");

    // Log options
    app.add_option("--log-level", config_.log.level, "Log level (debug, info, warning, error)");
    app.add_option("--log-output", config_.log.output, "Log output (stdout, stderr, file)");
    app.add_option("--log-file", config_.log.file_path, "Log file path");

    // Cache options
    app.add_option("--cache-memory-size", config_.cache.memory_cache_size_mb, "Memory cache size (MB)");
    app.add_option("--cache-disk-size", config_.cache.disk_cache_size_mb, "Disk cache size (MB)");
    app.add_option("--cache-chunk-size", config_.cache.chunk_size_mb, "Chunk size (MB)");
    app.add_option("--cache-disk-path", config_.cache.disk_cache_path, "Disk cache path");
    app.add_option("--cache-eviction-threshold", config_.cache.eviction_threshold, "Eviction threshold (0.0-1.0)");
    app.add_option("--cache-eviction-target", config_.cache.eviction_target, "Eviction target (0.0-1.0)");

    // P2P options
    app.add_option("--p2p-port", config_.p2p.listen_port, "P2P listen port");
    app.add_option("--p2p-max-connections", config_.p2p.max_connections, "Maximum P2P connections");
    app.add_option("--p2p-max-peers", config_.p2p.max_peers, "Maximum number of peers");
    app.add_option("--p2p-max-upload-speed", config_.p2p.max_upload_speed_mbps, "Maximum upload speed (Mbps, 0=unlimited)");
    app.add_option("--p2p-max-download-speed", config_.p2p.max_download_speed_mbps, "Maximum download speed (Mbps, 0=unlimited)");
    app.add_option("--p2p-selection-k", config_.p2p.selection_k, "Number of peers to select for transfer");
    app.add_option("--p2p-gossip-interval", config_.p2p.gossip_interval_sec, "Gossip interval (seconds)");
    app.add_option("--p2p-heartbeat-timeout", config_.p2p.heartbeat_timeout_sec, "Heartbeat timeout (seconds)");
    app.add_option("--p2p-transport", config_.p2p.transport, "Transport type (tcp, rdma)");

    // Control plane options
    app.add_option("--control-plane-endpoint", config_.control_plane.endpoint, "Control plane endpoint");
    app.add_option("--control-plane-port", config_.control_plane.port, "Control plane port");
    app.add_option("--control-plane-heartbeat", config_.control_plane.heartbeat_interval_sec, "Control plane heartbeat interval (seconds)");
    app.add_option("--control-plane-reconnect", config_.control_plane.reconnect_interval_sec, "Control plane reconnect interval (seconds)");
    app.add_flag("--control-plane-enable", config_.control_plane.enable, "Enable control plane");

    // Control plane server options
    app.add_option("--control-server-port", config_.control_plane_server.listen_port, "Control plane server listen port");
    app.add_option("--control-server-bind", config_.control_plane_server.bind_address, "Control plane server bind address");
    app.add_option("--control-server-timeout", config_.control_plane_server.heartbeat_timeout_sec, "Control plane server heartbeat timeout (seconds)");
    app.add_option("--control-server-max-nodes", config_.control_plane_server.max_nodes, "Control plane server max nodes");
    app.add_option("--control-server-min-replicas", config_.control_plane_server.min_replicas, "Control plane server min replicas per chunk");
    app.add_option("--control-server-max-replicas", config_.control_plane_server.max_replicas, "Control plane server max replicas per chunk");

    // Storage options
    app.add_option("--storage-type", config_.storage.type, "Storage type (s3, oss)");
    app.add_option("--storage-endpoint", config_.storage.endpoint, "Object storage endpoint URL");
    app.add_option("--storage-region", config_.storage.region, "Object storage region");
    app.add_option("--storage-bucket", config_.storage.bucket, "Object storage bucket");
    app.add_option("--storage-access-key", config_.storage.access_key, "Object storage access key");
    app.add_option("--storage-secret-key", config_.storage.secret_key, "Object storage secret key");
    app.add_flag("--storage-https", config_.storage.use_https, "Use HTTPS for storage");
    app.add_option("--storage-connection-timeout", config_.storage.connection_timeout_sec, "Storage connection timeout (seconds)");
    app.add_option("--storage-read-timeout", config_.storage.read_timeout_sec, "Storage read timeout (seconds)");

    // Proxy options
    app.add_option("--proxy-port", config_.proxy.listen_port, "HTTP proxy listen port");
    app.add_option("--proxy-bind-address", config_.proxy.bind_address, "HTTP proxy bind address");
    app.add_option("--proxy-max-connections", config_.proxy.max_connections, "Maximum proxy connections");
    app.add_option("--proxy-auth-type", config_.proxy.auth_type, "Proxy authentication type (none, basic, token)");
    app.add_option("--proxy-auth-token", config_.proxy.auth_token, "Proxy authentication token");

    // Add version option
    app.set_version_flag("-v,--version", "0.1.0");

    try {
        // Parse arguments - this will handle --help automatically
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // For help/version, we don't want to exit, just return false
        if (e.get_exit_code() == 0) {
            // This was a help or version request - the app already printed it
            return false;
        }
        // For other parse errors, exit immediately with the error code
        std::cerr << "Command line parse error: " << e.what() << std::endl;
        return false;
    }

    // Load config file if specified
    if (!config_file.empty()) {
        load_from_file(config_file);
    }

    // Apply log level from config
    if (!config_.log.level.empty()) {
        Logger::instance().set_level(parse_log_level(config_.log.level));
    }

    return true;
}

void Config::override_from_cmdline(int /*argc*/, char* /*argv*/[]) {
    // This function is kept for backward compatibility but is no longer used
    // The CLI11 parsing is done in parse_command_line() directly
}

bool Config::validate() const {
    if (config_.node.node_id.empty()) {
        Logger::instance().error("node_id is required");
        return false;
    }
    if (config_.proxy.listen_port == 0) {
        Logger::instance().error("proxy.listen_port must be set");
        return false;
    }
    return true;
}

void Config::print() const {
    Logger::instance().info("=== Configuration ===");
    Logger::instance().info("Node ID: " + config_.node.node_id);
    Logger::instance().info("Log Level: " + config_.log.level);
    Logger::instance().info("P2P Port: " + std::to_string(config_.p2p.listen_port));
    Logger::instance().info("Proxy Port: " + std::to_string(config_.proxy.listen_port));
    Logger::instance().info("Cache Size: " + std::to_string(config_.cache.memory_cache_size_mb) + " MB");
    Logger::instance().info("Control Plane: " + config_.control_plane.endpoint + ":" + std::to_string(config_.control_plane.port));
    Logger::instance().info("Storage Endpoint: " + config_.storage.endpoint);
}

} // namespace eliop2p
