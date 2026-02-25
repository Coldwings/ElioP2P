#include "eliop2p/control/client.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/error_code.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <random>
#include <nlohmann/json.hpp>

// Socket includes
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>

using json = nlohmann::json;

namespace eliop2p {

// Simple synchronous HTTP client using POSIX sockets
class SimpleHttpClient {
public:
    static std::pair<int, std::string> http_request(
        const std::string& host,
        uint16_t port,
        const std::string& method,
        const std::string& path,
        const std::string& body = "",
        const std::unordered_map<std::string, std::string>& headers = {}) {

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return {500, "Failed to create socket"};
        }

        struct hostent* server = gethostbyname(host.c_str());
        if (server == nullptr) {
            close(sock);
            return {500, "Failed to resolve host"};
        }

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        serv_addr.sin_port = htons(port);

        // Set timeout
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            return {500, "Failed to connect"};
        }

        // Build HTTP request
        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << ":" << port << "\r\n";

        for (const auto& h : headers) {
            request << h.first << ": " << h.second << "\r\n";
        }

        if (!body.empty()) {
            request << "Content-Type: application/json\r\n";
            request << "Content-Length: " << body.size() << "\r\n";
        }

        request << "Connection: close\r\n";
        request << "\r\n";

        if (!body.empty()) {
            request << body;
        }

        // Send request
        std::string request_str = request.str();
        ssize_t sent = send(sock, request_str.c_str(), request_str.size(), 0);
        if (sent < 0) {
            close(sock);
            return {500, "Failed to send request"};
        }

        // Read response
        std::string response;
        char buffer[4096];
        while (true) {
            ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            response += buffer;
        }

        close(sock);

        // Parse HTTP response
        size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return {500, "Invalid response"};
        }

        std::string status_line = response.substr(0, response.find("\r\n"));
        int status_code = 500;

        // Parse status code
        size_t pos = status_line.find(' ');
        if (pos != std::string::npos) {
            status_code = std::stoi(status_line.substr(pos + 1));
        }

        std::string response_body = response.substr(header_end + 4);
        return {status_code, response_body};
    }
};

struct ControlPlaneClient::Impl {
    ControlPlaneConfig config;
    std::atomic<bool> connected{false};
    std::atomic<bool> heartbeat_running{false};
    ChunkLocationsCallback update_callback;
    ReplicationCommandCallback replication_callback;
    std::thread heartbeat_thread;
    std::function<NodeStatus()> status_provider;
    std::string registered_node_id;
    std::atomic<bool> stop_heartbeat{false};

    Impl(const ControlPlaneConfig& cfg) : config(cfg) {}

    std::string get_base_url() const {
        return "http://" + config.endpoint + ":" + std::to_string(config.port);
    }

    bool check_connection() {
        if (!config.enable) {
            Logger::instance().warning("Control plane is disabled in config");
            return false;
        }

        auto [code, response] = SimpleHttpClient::http_request(
            config.endpoint,
            config.port,
            "GET",
            "/health");

        if (code == 200) {
            return true;
        }
        Logger::instance().warning("Control plane health check failed, code: " + std::to_string(code));
        return false;
    }
};

ControlPlaneClient::ControlPlaneClient(const ControlPlaneConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ControlPlaneClient::~ControlPlaneClient() {
    stop_heartbeat_loop();
    disconnect();
}

bool ControlPlaneClient::connect() {
    Logger::instance().info("Connecting to control plane: " + impl_->config.endpoint +
                           ":" + std::to_string(impl_->config.port));

    // Try to connect and check health
    if (!impl_->check_connection()) {
        Logger::instance().warning("Could not connect to control plane, will operate in degraded mode");
        // Don't fail - allow operation in degraded mode
        impl_->connected = true;
        return true;
    }

    impl_->connected = true;
    Logger::instance().info("Connected to control plane successfully");
    return true;
}

void ControlPlaneClient::disconnect() {
    if (impl_->connected.load()) {
        Logger::instance().info("Disconnected from control plane");
        impl_->connected = false;
    }
    stop_heartbeat_loop();
}

bool ControlPlaneClient::register_node(const NodeRegistration& registration) {
    if (!impl_->connected.load()) {
        Logger::instance().error("Cannot register: not connected to control plane");
        return false;
    }

    Logger::instance().info("Registering node: " + registration.node_id +
                           " at " + registration.address);

    try {
        json body = {
            {"node_id", registration.node_id},
            {"address", registration.address},
            {"p2p_port", registration.p2p_port},
            {"http_port", registration.http_port},
            {"memory_capacity_mb", registration.memory_capacity_mb},
            {"disk_capacity_mb", registration.disk_capacity_mb},
            {"available_chunks", registration.available_chunks}
        };

        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "POST",
            "/api/v1/nodes",
            body.dump(),
            {{"Content-Type", "application/json"}});

        if (code == 200 || code == 201) {
            Logger::instance().info("Node registered successfully: " + registration.node_id);
            impl_->registered_node_id = registration.node_id;
            return true;
        }

        Logger::instance().error("Failed to register node, HTTP code: " + std::to_string(code) +
                                ", response: " + response);
        return false;

    } catch (const std::exception& e) {
        Logger::instance().error("Exception during node registration: " + std::string(e.what()));
        return false;
    }
}

bool ControlPlaneClient::send_heartbeat(const NodeStatus& status) {
    if (!impl_->connected.load()) {
        return false;
    }

    try {
        json body = {
            {"node_id", status.node_id},
            {"online", status.online},
            {"memory_used_mb", status.memory_used_mb},
            {"disk_used_mb", status.disk_used_mb},
            {"cache_hit_rate", status.cache_hit_rate},
            {"total_chunks", status.total_chunks},
            {"active_connections", status.active_connections},
            {"upload_speed_mbps", status.upload_speed_mbps},
            {"download_speed_mbps", status.download_speed_mbps},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };

        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "PUT",
            "/api/v1/nodes/" + status.node_id + "/heartbeat",
            body.dump(),
            {{"Content-Type", "application/json"}});

        if (code == 200 || code == 204) {
            return true;
        }

        Logger::instance().warning("Heartbeat failed, code: " + std::to_string(code));
        return false;

    } catch (const std::exception& e) {
        Logger::instance().warning("Exception during heartbeat: " + std::string(e.what()));
        return false;
    }
}

bool ControlPlaneClient::report_chunks(const std::string& node_id,
                                      const std::vector<std::string>& chunk_ids) {
    if (!impl_->connected.load()) {
        return false;
    }

    try {
        json body = {
            {"chunk_ids", chunk_ids}
        };

        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "PUT",
            "/api/v1/nodes/" + node_id + "/chunks",
            body.dump(),
            {{"Content-Type", "application/json"}});

        if (code == 200 || code == 204) {
            Logger::instance().debug("Reported " + std::to_string(chunk_ids.size()) +
                                    " chunks for node: " + node_id);
            return true;
        }

        Logger::instance().warning("Failed to report chunks, code: " + std::to_string(code));
        return false;

    } catch (const std::exception& e) {
        Logger::instance().warning("Exception during chunk report: " + std::string(e.what()));
        return false;
    }
}

elio::coro::task<std::optional<std::vector<ChunkLocation>>> ControlPlaneClient::query_chunk_locations(
    const std::vector<std::string>& chunk_ids) {

    if (!impl_->connected.load()) {
        Logger::instance().warning("Cannot query chunk locations: not connected");
        co_return std::nullopt;
    }

    Logger::instance().debug("Querying chunk locations for " +
                             std::to_string(chunk_ids.size()) + " chunks");

    try {
        json body = {
            {"chunk_ids", chunk_ids}
        };

        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "POST",
            "/api/v1/chunks/locations",
            body.dump(),
            {{"Content-Type", "application/json"}});

        if (code == 200) {
            json result = json::parse(response);
            std::vector<ChunkLocation> locations;

            if (result.contains("locations")) {
                for (const auto& loc : result["locations"]) {
                    ChunkLocation cl;
                    cl.chunk_id = loc.value("chunk_id", "");
                    if (loc.contains("peer_ids")) {
                        for (const auto& peer : loc["peer_ids"]) {
                            cl.peer_ids.push_back(peer.get<std::string>());
                        }
                    }
                    locations.push_back(cl);
                }
            }

            Logger::instance().debug("Received " + std::to_string(locations.size()) +
                                    " chunk locations");
            co_return locations;
        }

        Logger::instance().warning("Query chunk locations failed, code: " + std::to_string(code));

    } catch (const std::exception& e) {
        Logger::instance().error("Exception during chunk location query: " + std::string(e.what()));
    }

    co_return std::nullopt;
}

elio::coro::task<std::optional<std::vector<NodeRegistration>>> ControlPlaneClient::query_nodes() {
    if (!impl_->connected.load()) {
        co_return std::nullopt;
    }

    try {
        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "GET",
            "/api/v1/nodes");

        if (code == 200) {
            json result = json::parse(response);
            std::vector<NodeRegistration> nodes;

            if (result.contains("nodes")) {
                for (const auto& n : result["nodes"]) {
                    NodeRegistration nr;
                    nr.node_id = n.value("node_id", "");
                    nr.address = n.value("address", "");
                    nr.p2p_port = n.value("p2p_port", 0);
                    nr.http_port = n.value("http_port", 0);
                    nr.memory_capacity_mb = n.value("memory_capacity_mb", 0);
                    nr.disk_capacity_mb = n.value("disk_capacity_mb", 0);
                    if (n.contains("available_chunks")) {
                        for (const auto& chunk : n["available_chunks"]) {
                            nr.available_chunks.push_back(chunk.get<std::string>());
                        }
                    }
                    nodes.push_back(nr);
                }
            }

            co_return nodes;
        }

    } catch (const std::exception& e) {
        Logger::instance().error("Exception during nodes query: " + std::string(e.what()));
    }

    co_return std::nullopt;
}

void ControlPlaneClient::subscribe_to_updates(ChunkLocationsCallback callback) {
    impl_->update_callback = std::move(callback);
}

void ControlPlaneClient::subscribe_to_replication_commands(ReplicationCommandCallback callback) {
    impl_->replication_callback = std::move(callback);
}

elio::coro::task<std::optional<ControlPlaneMetrics>> ControlPlaneClient::get_metrics() {
    if (!impl_->connected.load()) {
        co_return std::nullopt;
    }

    try {
        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "GET",
            "/api/v1/metrics");

        if (code == 200) {
            json result = json::parse(response);
            ControlPlaneMetrics metrics;

            metrics.total_nodes = result.value("total_nodes", 0);
            metrics.active_nodes = result.value("active_nodes", 0);
            metrics.total_chunks = result.value("total_chunks", 0);
            metrics.cache_hit_rate = result.value("cache_hit_rate", 0);

            co_return metrics;
        }

    } catch (const std::exception& e) {
        Logger::instance().error("Exception during metrics query: " + std::string(e.what()));
    }

    co_return std::nullopt;
}

bool ControlPlaneClient::is_connected() const {
    return impl_->connected.load();
}

void ControlPlaneClient::start_heartbeat_loop(const std::function<NodeStatus()>& status_provider) {
    if (impl_->heartbeat_running.load()) {
        Logger::instance().warning("Heartbeat loop already running");
        return;
    }

    impl_->status_provider = status_provider;
    impl_->stop_heartbeat = false;
    impl_->heartbeat_running = true;

    impl_->heartbeat_thread = std::thread([this]() {
        Logger::instance().info("Heartbeat loop started");

        while (!impl_->stop_heartbeat.load()) {
            try {
                if (impl_->status_provider && impl_->connected.load()) {
                    NodeStatus status = impl_->status_provider();
                    send_heartbeat(status);
                }

                // Sleep for the configured interval
                std::this_thread::sleep_for(
                    std::chrono::seconds(impl_->config.heartbeat_interval_sec));

            } catch (const std::exception& e) {
                Logger::instance().error("Error in heartbeat loop: " + std::string(e.what()));
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        Logger::instance().info("Heartbeat loop stopped");
        impl_->heartbeat_running = false;
    });
}

void ControlPlaneClient::stop_heartbeat_loop() {
    if (!impl_->heartbeat_running.load()) {
        return;
    }

    impl_->stop_heartbeat = true;
    if (impl_->heartbeat_thread.joinable()) {
        impl_->heartbeat_thread.join();
    }
    impl_->heartbeat_running = false;
}

bool ControlPlaneClient::is_heartbeat_running() const {
    return impl_->heartbeat_running.load();
}

elio::coro::task<std::optional<std::vector<ReplicationCommand>>> ControlPlaneClient::fetch_replication_commands() {
    if (!impl_->connected.load()) {
        co_return std::nullopt;
    }

    if (impl_->registered_node_id.empty()) {
        co_return std::nullopt;
    }

    try {
        auto [code, response] = SimpleHttpClient::http_request(
            impl_->config.endpoint,
            impl_->config.port,
            "GET",
            "/api/v1/nodes/" + impl_->registered_node_id + "/commands");

        if (code == 200) {
            json result = json::parse(response);
            std::vector<ReplicationCommand> commands;

            if (result.contains("commands")) {
                for (const auto& cmd : result["commands"]) {
                    ReplicationCommand rc;
                    rc.command_id = cmd.value("command_id", "");
                    rc.chunk_id = cmd.value("chunk_id", "");
                    rc.action = cmd.value("action", "");
                    rc.source_node_id = cmd.value("source_node_id", "");
                    rc.target_node_id = cmd.value("target_node_id", "");
                    rc.priority = cmd.value("priority", 0);
                    rc.created_at = cmd.value("created_at", 0);
                    commands.push_back(rc);
                }
            }

            if (!commands.empty()) {
                Logger::instance().info("Received " + std::to_string(commands.size()) +
                                        " replication commands");
            }

            co_return commands;
        }

    } catch (const std::exception& e) {
        Logger::instance().error("Exception fetching replication commands: " + std::string(e.what()));
    }

    co_return std::nullopt;
}

} // namespace eliop2p
