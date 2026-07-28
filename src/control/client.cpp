#include "eliop2p/control/client.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/base/error_code.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <iostream>
#include <random>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <mutex>
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

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

} // namespace

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

        // Send request (loop to handle partial sends)
        std::string request_str = request.str();
        size_t sent_total = 0;
        while (sent_total < request_str.size()) {
            ssize_t n = send(sock, request_str.c_str() + sent_total,
                             request_str.size() - sent_total, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                close(sock);
                return {500, "Failed to send request"};
            }
            if (n == 0) {
                close(sock);
                return {500, "Failed to send request"};
            }
            sent_total += static_cast<size_t>(n);
        }

        // Read the response incrementally: consume the header block first,
        // then frame the body via Content-Length or Transfer-Encoding:
        // chunked, falling back to read-until-EOF only when neither is
        // present.
        std::string response;
        char buffer[4096];
        auto read_more = [&]() -> bool {
            ssize_t n;
            do {
                n = recv(sock, buffer, sizeof(buffer), 0);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) return false;
            response.append(buffer, static_cast<size_t>(n));
            return true;
        };

        // Buffer at least `need` bytes; returns false on EOF/error.
        auto ensure_buffered = [&](size_t need) -> bool {
            while (response.size() < need) {
                if (!read_more()) return false;
            }
            return true;
        };

        // Read until the end of the header block.
        size_t header_end = std::string::npos;
        while ((header_end = response.find("\r\n\r\n")) == std::string::npos) {
            if (!read_more()) {
                close(sock);
                return {500, "Invalid response: incomplete headers"};
            }
        }

        // Parse the status line ("HTTP/1.1 200 OK"); guard against
        // malformed lines that would make std::stoi throw.
        int status_code = 500;
        size_t line_end = response.find("\r\n");
        try {
            size_t pos = response.find(' ');
            if (pos != std::string::npos && pos < line_end) {
                status_code = std::stoi(response.substr(pos + 1, line_end - pos - 1));
            }
        } catch (const std::exception&) {
            close(sock);
            return {500, "Invalid response: malformed status line"};
        }

        // Parse headers with case-insensitive keys.
        std::unordered_map<std::string, std::string> resp_headers;
        size_t hpos = (line_end == std::string::npos) ? header_end : line_end + 2;
        while (hpos < header_end) {
            size_t hend = response.find("\r\n", hpos);
            if (hend == std::string::npos || hend > header_end) {
                hend = header_end;
            }
            std::string line = response.substr(hpos, hend - hpos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                resp_headers[to_lower(trim(line.substr(0, colon)))] =
                    trim(line.substr(colon + 1));
            }
            hpos = hend + 2;
        }

        const size_t body_start = header_end + 4;
        std::string response_body;

        bool chunked = false;
        auto te_it = resp_headers.find("transfer-encoding");
        if (te_it != resp_headers.end() &&
            to_lower(te_it->second).find("chunked") != std::string::npos) {
            chunked = true;
        }

        if (chunked) {
            // Decode chunked transfer-encoding.
            size_t pos = body_start;
            while (true) {
                size_t eol = std::string::npos;
                while ((eol = response.find("\r\n", pos)) == std::string::npos) {
                    if (!read_more()) {
                        close(sock);
                        return {500, "Invalid response: truncated chunk header"};
                    }
                }
                std::string size_field = response.substr(pos, eol - pos);
                size_t semi = size_field.find(';');  // ignore chunk extensions
                if (semi != std::string::npos) {
                    size_field = size_field.substr(0, semi);
                }
                size_t chunk_size = 0;
                try {
                    chunk_size = std::stoul(trim(size_field), nullptr, 16);
                } catch (const std::exception&) {
                    close(sock);
                    return {500, "Invalid response: malformed chunk size"};
                }
                pos = eol + 2;
                if (chunk_size == 0) {
                    // Terminal chunk; trailers (if any) are ignored.
                    break;
                }
                if (!ensure_buffered(pos + chunk_size + 2)) {  // data + CRLF
                    close(sock);
                    return {500, "Invalid response: truncated chunk data"};
                }
                response_body.append(response, pos, chunk_size);
                pos += chunk_size + 2;
            }
        } else {
            auto cl_it = resp_headers.find("content-length");
            if (cl_it != resp_headers.end()) {
                size_t content_length = 0;
                try {
                    content_length = std::stoul(cl_it->second);
                } catch (const std::exception&) {
                    close(sock);
                    return {500, "Invalid response: malformed Content-Length"};
                }
                if (!ensure_buffered(body_start + content_length)) {
                    close(sock);
                    return {500, "Invalid response: truncated body"};
                }
                response_body = response.substr(body_start, content_length);
            } else {
                // No framing information: read until the server closes.
                while (read_more()) {}
                response_body = response.substr(body_start);
            }
        }

        close(sock);
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
    std::mutex node_id_mutex;       // guards registered_node_id
    std::mutex heartbeat_mutex;     // serializes start/stop of the heartbeat loop
    std::atomic<bool> stop_heartbeat{false};
    std::shared_ptr<elio::runtime::scheduler> scheduler;

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
            {
                std::lock_guard<std::mutex> lock(impl_->node_id_mutex);
                impl_->registered_node_id = registration.node_id;
            }
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

        const std::string endpoint = impl_->config.endpoint;
        const uint16_t port = impl_->config.port;
        const std::string request_body = body.dump();

        // Offload the blocking HTTP call to the blocking thread pool so the
        // scheduler worker thread is not stalled.
        auto [code, response] = co_await elio::spawn_blocking(
            [endpoint, port, request_body]() {
                return SimpleHttpClient::http_request(
                    endpoint, port, "POST", "/api/v1/chunks/locations",
                    request_body, {{"Content-Type", "application/json"}});
            });

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
        const std::string endpoint = impl_->config.endpoint;
        const uint16_t port = impl_->config.port;

        // Offload the blocking HTTP call to the blocking thread pool so the
        // scheduler worker thread is not stalled.
        auto [code, response] = co_await elio::spawn_blocking(
            [endpoint, port]() {
                return SimpleHttpClient::http_request(
                    endpoint, port, "GET", "/api/v1/nodes");
            });

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
        const std::string endpoint = impl_->config.endpoint;
        const uint16_t port = impl_->config.port;

        // Offload the blocking HTTP call to the blocking thread pool so the
        // scheduler worker thread is not stalled.
        auto [code, response] = co_await elio::spawn_blocking(
            [endpoint, port]() {
                return SimpleHttpClient::http_request(
                    endpoint, port, "GET", "/api/v1/metrics");
            });

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
    std::lock_guard<std::mutex> lock(impl_->heartbeat_mutex);

    // Atomic check-and-set: only one concurrent caller may start the loop.
    if (impl_->heartbeat_running.exchange(true)) {
        Logger::instance().warning("Heartbeat loop already running");
        return;
    }

    impl_->status_provider = status_provider;
    impl_->stop_heartbeat = false;

    // Clamp the interval: 0 would degrade the loop into a busy wait.
    const uint32_t interval_sec = std::max<uint32_t>(impl_->config.heartbeat_interval_sec, 1);

    try {
        impl_->heartbeat_thread = std::thread([this, interval_sec]() {
            Logger::instance().info("Heartbeat loop started");

            while (!impl_->stop_heartbeat.load()) {
                try {
                    if (impl_->status_provider && impl_->connected.load()) {
                        NodeStatus status = impl_->status_provider();
                        send_heartbeat(status);
                    }

                    // Sleep for the configured interval (at least 1 second)
                    std::this_thread::sleep_for(std::chrono::seconds(interval_sec));

                } catch (const std::exception& e) {
                    Logger::instance().error("Error in heartbeat loop: " + std::string(e.what()));
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
            }

            Logger::instance().info("Heartbeat loop stopped");
            // Note: heartbeat_running is cleared by stop_heartbeat_loop()
            // after joining this thread, never from within the thread itself.
        });
    } catch (...) {
        impl_->heartbeat_running = false;
        throw;
    }
}

void ControlPlaneClient::stop_heartbeat_loop() {
    std::lock_guard<std::mutex> lock(impl_->heartbeat_mutex);

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

    std::string node_id;
    {
        std::lock_guard<std::mutex> lock(impl_->node_id_mutex);
        node_id = impl_->registered_node_id;
    }
    if (node_id.empty()) {
        co_return std::nullopt;
    }

    try {
        const std::string endpoint = impl_->config.endpoint;
        const uint16_t port = impl_->config.port;

        // Offload the blocking HTTP call to the blocking thread pool so the
        // scheduler worker thread is not stalled.
        auto [code, response] = co_await elio::spawn_blocking(
            [endpoint, port, node_id]() {
                return SimpleHttpClient::http_request(
                    endpoint, port, "GET",
                    "/api/v1/nodes/" + node_id + "/commands");
            });

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

                // Deliver to the subscribed handler FIRST: the ACK below
                // only happens after processing, so a crash in between
                // causes the server to redeliver (at-least-once semantics).
                if (impl_->replication_callback) {
                    try {
                        impl_->replication_callback(commands);
                    } catch (const std::exception& e) {
                        Logger::instance().error("Replication command callback failed: " +
                                                 std::string(e.what()));
                        // Do NOT ACK: processing failed, let the server redeliver
                        co_return commands;
                    }
                }

                // Acknowledge each command so the server stops redelivering it
                for (const auto& cmd : commands) {
                    if (!cmd.command_id.empty()) {
                        co_await ack_command(cmd.command_id);
                    }
                }
            }

            co_return commands;
        }

    } catch (const std::exception& e) {
        Logger::instance().error("Exception fetching replication commands: " + std::string(e.what()));
    }

    co_return std::nullopt;
}

void ControlPlaneClient::set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) {
    impl_->scheduler = scheduler;
}


elio::coro::task<bool> ControlPlaneClient::ack_command(const std::string& command_id) {
    if (!impl_->connected.load() || command_id.empty()) {
        co_return false;
    }

    std::string node_id;
    {
        std::lock_guard<std::mutex> lock(impl_->node_id_mutex);
        node_id = impl_->registered_node_id;
    }
    if (node_id.empty()) {
        co_return false;
    }

    const std::string endpoint = impl_->config.endpoint;
    const uint16_t port = impl_->config.port;

    auto [code, response] = co_await elio::spawn_blocking(
        [endpoint, port, node_id, command_id]() {
            json ack_body = {{"command_id", command_id}};
            return SimpleHttpClient::http_request(
                endpoint, port, "POST",
                "/api/v1/nodes/" + node_id + "/commands/ack",
                ack_body.dump(),
                {{"Content-Type", "application/json"}});
        });

    if (code != 200 && code != 204) {
        Logger::instance().warning("Failed to ack replication command " + command_id +
                                   ", code: " + std::to_string(code));
        co_return false;
    }
    co_return true;
}

} // namespace eliop2p
