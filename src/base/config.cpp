#include "eliop2p/base/config.h"
#include "eliop2p/base/logger.h"
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
    if (level == "debug") return LogLevel::DEBUG;
    if (level == "info") return LogLevel::INFO;
    if (level == "warning") return LogLevel::WARNING;
    if (level == "error") return LogLevel::ERROR;
    if (level == "fatal") return LogLevel::FATAL;
    return LogLevel::INFO;
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
    override_from_cmdline(argc, argv);
    return true;
}

void Config::override_from_cmdline(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Parse --config=path or -c path
        if (arg == "--config" || arg == "-c") {
            if (i + 1 < argc) {
                load_from_file(argv[++i]);
            }
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) {
            load_from_file(arg.substr(9));
            continue;
        }

        // Parse --node-id=value
        if (arg.rfind("--node-id=", 0) == 0) {
            config_.node.node_id = arg.substr(11);
            continue;
        }

        // Parse --log-level=value
        if (arg.rfind("--log-level=", 0) == 0) {
            config_.log.level = arg.substr(12);
            continue;
        }

        // Parse --p2p-port=value
        if (arg.rfind("--p2p-port=", 0) == 0) {
            config_.p2p.listen_port = std::stoi(arg.substr(11));
            continue;
        }

        // Parse --proxy-port=value
        if (arg.rfind("--proxy-port=", 0) == 0) {
            config_.proxy.listen_port = std::stoi(arg.substr(12));
            continue;
        }

        // Parse --control-plane=value
        if (arg.rfind("--control-plane=", 0) == 0) {
            config_.control_plane.endpoint = arg.substr(16);
            continue;
        }

        // Parse --storage-endpoint=value
        if (arg.rfind("--storage-endpoint=", 0) == 0) {
            config_.storage.endpoint = arg.substr(19);
            continue;
        }

        // Parse --cache-size=value (MB)
        if (arg.rfind("--cache-size=", 0) == 0) {
            config_.cache.memory_cache_size_mb = std::stoull(arg.substr(12));
            continue;
        }
    }
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
