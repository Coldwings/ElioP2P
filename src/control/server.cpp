#include "eliop2p/control/server.h"
#include "eliop2p/base/logger.h"
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>
#include <unistd.h>
#include <sys/socket.h>
#include <random>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace eliop2p {

// Node information stored in server
struct NodeInfo {
    NodeRegistration registration;
    NodeStatus status;
    uint64_t last_heartbeat_time = 0;
    bool online = false;
    std::vector<std::string> chunks;
};

struct ControlPlaneServer::Impl {
    ControlPlaneServerConfig config;
    std::atomic<bool> running{false};
    ControlPlaneServerMetrics metrics;
    mutable std::mutex metrics_mutex;
    mutable std::mutex nodes_mutex;

    // Node registry: node_id -> NodeInfo
    std::unordered_map<std::string, NodeInfo> nodes;

    // Chunk location index: chunk_id -> set of node_ids
    std::unordered_map<std::string, std::unordered_set<std::string>> chunk_index;

    // Pending replication commands for each node
    std::unordered_map<std::string, std::vector<ReplicationCommand>> pending_commands;

    // Metadata manager for persistent storage
    std::unique_ptr<MetadataManager> metadata_manager;

    // Server thread
    std::thread server_thread;

    // HTTP server instance
    std::unique_ptr<elio::http::server> http_server;

    // Scheduler for coroutines
    std::shared_ptr<elio::runtime::scheduler> scheduler;

    // Pipe for stop notification
    int stop_pipe[2] = {-1, -1};

    // Shared pointer to this for lambdas
    std::shared_ptr<Impl> self;

    Impl(const ControlPlaneServerConfig& cfg) : config(cfg) {
        // Create pipe for stop notification
        if (pipe(stop_pipe) == -1) {
            Logger::instance().error("Failed to create stop pipe for control plane server");
        }

        metadata_manager = std::make_unique<MetadataManager>();
    }

    // Check if node is active (heartbeat within timeout)
    bool is_node_active(const NodeInfo& node) const {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return (now - node.last_heartbeat_time) <= config.heartbeat_timeout_sec;
    }

    // Update chunk index when node reports chunks
    void update_chunk_index(const std::string& node_id,
                           const std::vector<std::string>& chunk_ids,
                           bool add) {
        std::lock_guard<std::mutex> lock(nodes_mutex);

        if (add) {
            for (const auto& chunk_id : chunk_ids) {
                chunk_index[chunk_id].insert(node_id);
            }
        } else {
            for (const auto& chunk_id : chunk_ids) {
                auto it = chunk_index.find(chunk_id);
                if (it != chunk_index.end()) {
                    it->second.erase(node_id);
                    if (it->second.empty()) {
                        chunk_index.erase(it);
                    }
                }
            }
        }
    }
};

// Helper function to parse JSON body from request
static json parse_json_body(const elio::http::request& req) {
    auto body = req.body();
    std::string body_str(body.begin(), body.end());

    if (body_str.empty()) {
        return json::object();
    }

    try {
        return json::parse(body_str);
    } catch (const std::exception& e) {
        Logger::instance().warning("Failed to parse JSON body: " + std::string(e.what()));
        return json::object();
    }
}

ControlPlaneServer::ControlPlaneServer(const ControlPlaneServerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ControlPlaneServer::~ControlPlaneServer() {
    stop();
}

bool ControlPlaneServer::start() {
    Logger::instance().info("Starting control plane server on " +
                            impl_->config.bind_address + ":" +
                            std::to_string(impl_->config.listen_port));

    impl_->running = true;

    // Keep a shared_ptr to impl for the lambdas
    auto self = std::shared_ptr<Impl>(impl_.get(), [](Impl*){});

    // Create router
    elio::http::router r;

    // Health check
    r.get("/health", [self](elio::http::context&)
          -> elio::coro::task<elio::http::response> {
        json response = {
            {"status", "healthy"},
            {"service", "control-plane"},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };
        co_return elio::http::response(elio::http::status::ok, response.dump(),
                                       elio::http::mime::application_json);
    });

    // Register node
    r.post("/api/v1/nodes", [self](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        auto body = parse_json_body(ctx.req());

        if (!body.contains("node_id") || !body.contains("address")) {
            json error = {
                {"error", "Missing required fields: node_id, address"}
            };
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        NodeRegistration reg;
        reg.node_id = body.value("node_id", "");
        reg.address = body.value("address", "");
        reg.p2p_port = body.value("p2p_port", 0);
        reg.http_port = body.value("http_port", 0);
        reg.memory_capacity_mb = body.value("memory_capacity_mb", 0);
        reg.disk_capacity_mb = body.value("disk_capacity_mb", 0);

        if (body.contains("available_chunks")) {
            for (const auto& chunk : body["available_chunks"]) {
                reg.available_chunks.push_back(chunk.get<std::string>());
            }
        }

        {
            std::lock_guard<std::mutex> lock(self->nodes_mutex);

            // Check if node already exists
            auto it = self->nodes.find(reg.node_id);
            if (it != self->nodes.end()) {
                // Update existing node
                it->second.registration = reg;
                it->second.status.node_id = reg.node_id;
                it->second.last_heartbeat_time = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                it->second.online = true;

                Logger::instance().info("Updated existing node: " + reg.node_id);
            } else {
                // Register new node
                if (self->nodes.size() >= self->config.max_nodes) {
                    json error = {{"error", "Maximum node limit reached"}};
                    co_return elio::http::response(elio::http::status::service_unavailable,
                                                   error.dump(), elio::http::mime::application_json);
                }

                NodeInfo node_info;
                node_info.registration = reg;
                node_info.status.node_id = reg.node_id;
                node_info.status.online = true;
                node_info.last_heartbeat_time = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                node_info.online = true;

                self->nodes[reg.node_id] = std::move(node_info);
                Logger::instance().info("Registered new node: " + reg.node_id);
            }

            // Update chunk index
            self->update_chunk_index(reg.node_id, reg.available_chunks, true);

            // Update metrics
            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_nodes = self->nodes.size();
        }

        json response = {
            {"success", true},
            {"node_id", reg.node_id},
            {"message", "Node registered successfully"}
        };

        co_return elio::http::response(elio::http::status::created, response.dump(),
                                        elio::http::mime::application_json);
    });

    // Node heartbeat
    r.put("/api/v1/nodes/:node_id/heartbeat", [self](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        // Get path parameter from the request path
        std::string req_path = std::string(ctx.req().path());
        std::string node_id;

        // Parse node_id from path: /api/v1/nodes/{node_id}/heartbeat
        size_t nodes_pos = req_path.find("/api/v1/nodes/");
        if (nodes_pos != std::string::npos) {
            size_t start = nodes_pos + 15;
            size_t end = req_path.find("/heartbeat", start);
            if (end != std::string::npos) {
                node_id = req_path.substr(start, end - start);
            }
        }

        if (node_id.empty()) {
            json error = {{"error", "Invalid path"}};
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        auto body = parse_json_body(ctx.req());

        {
            std::lock_guard<std::mutex> lock(self->nodes_mutex);

            auto it = self->nodes.find(node_id);
            if (it == self->nodes.end()) {
                json error = {{"error", "Node not found: " + node_id}};
                co_return elio::http::response(elio::http::status::not_found, error.dump(),
                                               elio::http::mime::application_json);
            }

            // Update node status
            it->second.status.online = body.value("online", true);
            it->second.status.memory_used_mb = body.value("memory_used_mb", 0);
            it->second.status.disk_used_mb = body.value("disk_used_mb", 0);
            it->second.status.cache_hit_rate = body.value("cache_hit_rate", 0.0f);
            it->second.status.total_chunks = body.value("total_chunks", 0);
            it->second.status.active_connections = body.value("active_connections", 0);
            it->second.status.upload_speed_mbps = body.value("upload_speed_mbps", 0.0);
            it->second.status.download_speed_mbps = body.value("download_speed_mbps", 0.0);
            it->second.status.last_update_time = body.value("timestamp", 0);
            it->second.last_heartbeat_time = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            it->second.online = true;

            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_heartbeats++;
        }

        co_return elio::http::response(elio::http::status::no_content, "",
                                       elio::http::mime::application_json);
    });

    // Report chunks
    r.put("/api/v1/nodes/:node_id/chunks", [self](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        // Get path parameter from the request path
        std::string req_path = std::string(ctx.req().path());
        std::string node_id;

        // Parse node_id from path: /api/v1/nodes/{node_id}/chunks
        size_t nodes_pos = req_path.find("/api/v1/nodes/");
        if (nodes_pos != std::string::npos) {
            size_t start = nodes_pos + 15;
            size_t end = req_path.find("/chunks", start);
            if (end != std::string::npos) {
                node_id = req_path.substr(start, end - start);
            }
        }

        if (node_id.empty()) {
            json error = {{"error", "Invalid path"}};
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        auto body = parse_json_body(ctx.req());

        if (!body.contains("chunk_ids")) {
            json error = {{"error", "Missing chunk_ids"}};
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        std::vector<std::string> chunk_ids;
        for (const auto& chunk : body["chunk_ids"]) {
            chunk_ids.push_back(chunk.get<std::string>());
        }

        {
            std::lock_guard<std::mutex> lock(self->nodes_mutex);

            auto it = self->nodes.find(node_id);
            if (it == self->nodes.end()) {
                json error = {{"error", "Node not found: " + node_id}};
                co_return elio::http::response(elio::http::status::not_found, error.dump(),
                                               elio::http::mime::application_json);
            }

            // Update node's chunk list
            self->update_chunk_index(node_id, it->second.chunks, false);
            self->update_chunk_index(node_id, chunk_ids, true);
            it->second.chunks = chunk_ids;

            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_chunk_reports++;
        }

        co_return elio::http::response(elio::http::status::no_content, "",
                                       elio::http::mime::application_json);
    });

    // Query all nodes
    r.get("/api/v1/nodes", [self](elio::http::context&)
           -> elio::coro::task<elio::http::response> {
        std::lock_guard<std::mutex> lock(self->nodes_mutex);

        json nodes_json = json::array();
        size_t active_count = 0;

        for (const auto& [node_id, node_info] : self->nodes) {
            bool is_active = self->is_node_active(node_info);

            json node_json = {
                {"node_id", node_info.registration.node_id},
                {"address", node_info.registration.address},
                {"p2p_port", node_info.registration.p2p_port},
                {"http_port", node_info.registration.http_port},
                {"memory_capacity_mb", node_info.registration.memory_capacity_mb},
                {"disk_capacity_mb", node_info.registration.disk_capacity_mb},
                {"available_chunks", node_info.chunks},
                {"online", is_active}
            };

            if (is_active) {
                node_json["memory_used_mb"] = node_info.status.memory_used_mb;
                node_json["disk_used_mb"] = node_info.status.disk_used_mb;
                node_json["cache_hit_rate"] = node_info.status.cache_hit_rate;
                node_json["total_chunks"] = node_info.status.total_chunks;
                node_json["active_connections"] = node_info.status.active_connections;
                active_count++;
            }

            nodes_json.push_back(node_json);
        }

        {
            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_nodes = self->nodes.size();
            self->metrics.active_nodes = active_count;
        }

        json response = {
            {"nodes", nodes_json},
            {"total", self->nodes.size()},
            {"active", active_count}
        };

        co_return elio::http::response(elio::http::status::ok, response.dump(),
                                       elio::http::mime::application_json);
    });

    // Query chunk locations
    r.post("/api/v1/chunks/locations", [self](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        auto body = parse_json_body(ctx.req());

        if (!body.contains("chunk_ids")) {
            json error = {{"error", "Missing chunk_ids"}};
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        std::vector<std::string> requested_chunks;
        for (const auto& chunk : body["chunk_ids"]) {
            requested_chunks.push_back(chunk.get<std::string>());
        }

        std::lock_guard<std::mutex> lock(self->nodes_mutex);

        json locations_json = json::array();

        for (const auto& chunk_id : requested_chunks) {
            json location_json;
            location_json["chunk_id"] = chunk_id;

            auto it = self->chunk_index.find(chunk_id);
            if (it != self->chunk_index.end()) {
                json peer_ids = json::array();
                for (const auto& peer_id : it->second) {
                    peer_ids.push_back(peer_id);
                }
                location_json["peer_ids"] = peer_ids;
            } else {
                location_json["peer_ids"] = json::array();
            }

            locations_json.push_back(location_json);
        }

        {
            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_queries++;
        }

        json response = {
            {"locations", locations_json}
        };

        co_return elio::http::response(elio::http::status::ok, response.dump(),
                                       elio::http::mime::application_json);
    });

    // Get metrics
    r.get("/api/v1/metrics", [self](elio::http::context&)
           -> elio::coro::task<elio::http::response> {
        std::lock_guard<std::mutex> lock(self->nodes_mutex);

        size_t active_count = 0;
        for (const auto& [node_id, node_info] : self->nodes) {
            if (self->is_node_active(node_info)) {
                active_count++;
            }
        }

        {
            std::lock_guard<std::mutex> metrics_lock(self->metrics_mutex);
            self->metrics.total_nodes = self->nodes.size();
            self->metrics.active_nodes = active_count;

            json response = {
                {"total_nodes", self->metrics.total_nodes},
                {"active_nodes", self->metrics.active_nodes},
                {"total_chunks", self->chunk_index.size()},
                {"total_heartbeats", self->metrics.total_heartbeats},
                {"total_chunk_reports", self->metrics.total_chunk_reports},
                {"total_queries", self->metrics.total_queries},
                {"commands_issued", self->metrics.commands_issued},
                {"replication_decisions", self->metrics.replication_decisions}
            };

            co_return elio::http::response(elio::http::status::ok, response.dump(),
                                           elio::http::mime::application_json);
        }
    });

    // Get replication commands for node
    r.get("/api/v1/nodes/:node_id/commands", [self](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        // Get path parameter from the request path
        std::string req_path = std::string(ctx.req().path());
        std::string node_id;

        // Parse node_id from path: /api/v1/nodes/{node_id}/commands
        size_t nodes_pos = req_path.find("/api/v1/nodes/");
        if (nodes_pos != std::string::npos) {
            size_t start = nodes_pos + 15;
            size_t end = req_path.find("/commands", start);
            if (end != std::string::npos) {
                node_id = req_path.substr(start, end - start);
            }
        }

        if (node_id.empty()) {
            json error = {{"error", "Invalid path"}};
            co_return elio::http::response(elio::http::status::bad_request, error.dump(),
                                           elio::http::mime::application_json);
        }

        std::lock_guard<std::mutex> lock(self->nodes_mutex);

        auto it = self->nodes.find(node_id);
        if (it == self->nodes.end()) {
            json error = {{"error", "Node not found: " + node_id}};
            co_return elio::http::response(elio::http::status::not_found, error.dump(),
                                           elio::http::mime::application_json);
        }

        // Check for pending commands
        auto cmd_it = self->pending_commands.find(node_id);
        if (cmd_it == self->pending_commands.end()) {
            json response = {{"commands", json::array()}};
            co_return elio::http::response(elio::http::status::ok, response.dump(),
                                           elio::http::mime::application_json);
        }

        json commands_json = json::array();
        for (const auto& cmd : cmd_it->second) {
            json cmd_json;
            cmd_json["command_id"] = cmd.command_id;
            cmd_json["chunk_id"] = cmd.chunk_id;
            cmd_json["action"] = cmd.action;
            cmd_json["source_node_id"] = cmd.source_node_id;
            cmd_json["target_node_id"] = cmd.target_node_id;
            cmd_json["priority"] = cmd.priority;
            cmd_json["created_at"] = cmd.created_at;
            commands_json.push_back(cmd_json);
        }

        // Clear pending commands after delivery
        self->pending_commands.erase(cmd_it);

        json response = {{"commands", commands_json}};

        co_return elio::http::response(elio::http::status::ok, response.dump(),
                                       elio::http::mime::application_json);
    });

    // Create HTTP server with router
    impl_->http_server = std::make_unique<elio::http::server>(std::move(r));

    // Set not found handler
    impl_->http_server->set_not_found_handler([](elio::http::context& ctx)
        -> elio::coro::task<elio::http::response> {
        std::string json_str = R"({"error": "Not Found", "path": ")" +
                          std::string(ctx.req().path()) + R"("})";
        co_return elio::http::response(elio::http::status::not_found, json_str,
                                       elio::http::mime::application_json);
    });

    // Determine bind address
    elio::net::socket_address bind_addr;
    if (impl_->config.bind_address == "0.0.0.0") {
        bind_addr = elio::net::socket_address(elio::net::ipv6_address(impl_->config.listen_port));
    } else {
        bind_addr = elio::net::socket_address(impl_->config.bind_address, impl_->config.listen_port);
    }

    elio::net::tcp_options opts;
    opts.ipv6_only = (impl_->config.bind_address != "0.0.0.0");

    // Start HTTP server in a separate thread using elio scheduler
    impl_->server_thread = std::thread([this, bind_addr, opts]() {
        Logger::instance().info("Control plane HTTP server thread started");

        elio::run([this, bind_addr, opts]() -> elio::coro::task<void> {
            auto listen_task = impl_->http_server->listen(bind_addr, opts);
            auto listen_handle = std::move(listen_task).spawn();

            // Wait for running flag to become false
            while (impl_->running) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(100));
            }

            // Stop the HTTP server
            Logger::instance().info("Stopping control plane HTTP server");
            impl_->http_server->stop();
            // Wait for listen task to complete
            co_await listen_handle;

            Logger::instance().info("Control plane HTTP server stopped");
            co_return;
        }());

        Logger::instance().info("Control plane HTTP server thread exiting");
    });

    // Give the server thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Logger::instance().info("Control plane server started successfully");
    return true;
}

void ControlPlaneServer::stop() {
    if (impl_->running) {
        Logger::instance().info("Stopping control plane server - setting running to false");
        impl_->running = false;

        // Wait for server thread to finish briefly
        if (impl_->server_thread.joinable()) {
            constexpr auto timeout = std::chrono::seconds(1);
            auto start = std::chrono::steady_clock::now();

            while (impl_->server_thread.joinable()) {
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= timeout) {
                    Logger::instance().warning("Control plane server thread join timeout, detaching");
                    impl_->server_thread.detach();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        Logger::instance().info("Control plane server stopped");
    }
}

bool ControlPlaneServer::is_running() const {
    return impl_->running.load();
}

ControlPlaneServerMetrics ControlPlaneServer::get_metrics() const {
    std::lock_guard<std::mutex> lock(impl_->metrics_mutex);
    return impl_->metrics;
}

uint16_t ControlPlaneServer::get_listen_port() const {
    return impl_->config.listen_port;
}

size_t ControlPlaneServer::get_node_count() const {
    std::lock_guard<std::mutex> lock(impl_->nodes_mutex);
    return impl_->nodes.size();
}

size_t ControlPlaneServer::get_active_node_count() const {
    std::lock_guard<std::mutex> lock(impl_->nodes_mutex);
    size_t count = 0;
    for (const auto& [node_id, node_info] : impl_->nodes) {
        if (impl_->is_node_active(node_info)) {
            count++;
        }
    }
    return count;
}

std::vector<NodeRegistration> ControlPlaneServer::get_all_nodes() const {
    std::lock_guard<std::mutex> lock(impl_->nodes_mutex);
    std::vector<NodeRegistration> result;
    for (const auto& [node_id, node_info] : impl_->nodes) {
        result.push_back(node_info.registration);
    }
    return result;
}

std::optional<std::vector<ChunkLocation>> ControlPlaneServer::query_chunk_locations(
    const std::vector<std::string>& chunk_ids) const {

    std::lock_guard<std::mutex> lock(impl_->nodes_mutex);

    std::vector<ChunkLocation> locations;
    for (const auto& chunk_id : chunk_ids) {
        ChunkLocation loc;
        loc.chunk_id = chunk_id;

        auto it = impl_->chunk_index.find(chunk_id);
        if (it != impl_->chunk_index.end()) {
            loc.peer_ids.assign(it->second.begin(), it->second.end());
        }

        locations.push_back(loc);
    }

    return locations;
}

void ControlPlaneServer::set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) {
    impl_->scheduler = scheduler;
}

} // namespace eliop2p
