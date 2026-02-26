#include "eliop2p/proxy/server.h"
#include "eliop2p/proxy/request_handler.h"
#include "eliop2p/base/logger.h"
#include <elio/elio.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/http/http.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <functional>
#include <unistd.h>
#include <sys/socket.h>

namespace eliop2p {

// Forward declare TransferManager
class TransferManager;

struct ProxyServer::Impl {
    ProxyConfig config;
    std::atomic<bool> running{false};
    ProxyMetrics metrics;
    std::mutex metrics_mutex;
    std::shared_ptr<ChunkManager> cache_manager;
    std::shared_ptr<StorageClient> storage_client;
    std::shared_ptr<RequestHandler> request_handler;
    std::thread server_thread;
    std::shared_ptr<TransferManager> transfer_manager;
    bool enable_p2p_fallback = true;

    // Server instance for elio::serve
    std::unique_ptr<elio::http::server> http_server;

    // Pipe for stop notification
    int stop_pipe[2] = {-1, -1};
};

ProxyServer::ProxyServer(const ProxyConfig& config,
                         std::shared_ptr<ChunkManager> cache_manager,
                         std::shared_ptr<StorageClient> storage_client)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->cache_manager = cache_manager;
    impl_->storage_client = storage_client;
    impl_->request_handler = std::make_shared<RequestHandler>(cache_manager, storage_client);

    // Create pipe for stop notification
    if (pipe(impl_->stop_pipe) == -1) {
        Logger::instance().error("Failed to create stop pipe");
    }
}

ProxyServer::~ProxyServer() {
    stop();
}

// Convert Elio request to HttpRequest
static HttpRequest convert_request(const elio::http::request& req) {
    HttpRequest http_req;
    http_req.method = std::string(elio::http::method_to_string(req.get_method()));
    http_req.path = std::string(req.path());

    // Extract query string from query()
    std::string query = std::string(req.query());
    http_req.query_string = query;

    // Copy headers using get_headers()
    const auto& headers = req.get_headers();
    for (const auto& [key, value] : headers) {
        http_req.headers[std::string(key)] = std::string(value);
    }

    // Copy body
    auto body = req.body();
    http_req.body.assign(body.begin(), body.end());

    return http_req;
}

// Convert HttpResponse to Elio response
static elio::http::response convert_response(const HttpResponse& resp) {
    elio::http::response elio_resp;
    elio_resp.set_status(elio::http::status(resp.status_code));

    for (const auto& [key, value] : resp.headers) {
        elio_resp.set_header(key, value);
    }

    elio_resp.set_body(std::string(resp.body.begin(), resp.body.end()));

    return elio_resp;
}

// HTTP request handler that wraps RequestHandler
static elio::coro::task<elio::http::response> proxy_request_handler(
    elio::http::context& ctx,
    std::shared_ptr<RequestHandler> request_handler) {

    auto& elio_req = ctx.req();
    HttpRequest http_req = convert_request(elio_req);

    // Handle the request through RequestHandler
    auto http_resp = co_await request_handler->handle_request(http_req);

    co_return convert_response(http_resp);
}

bool ProxyServer::start() {
    Logger::instance().info("Starting proxy server on " +
                            impl_->config.bind_address + ":" +
                            std::to_string(impl_->config.listen_port));

    impl_->running = true;

    // Create router
    elio::http::router r;

    // Register catch-all route for proxy (handles all paths like /bucket/key)
    r.get("/", [this](elio::http::context& ctx)
          -> elio::coro::task<elio::http::response> {
        co_return co_await proxy_request_handler(ctx, impl_->request_handler);
    });

    // Register catch-all route for any path (bucket/key format)
    r.get("/:path", [this](elio::http::context& ctx)
          -> elio::coro::task<elio::http::response> {
        co_return co_await proxy_request_handler(ctx, impl_->request_handler);
    });

    // Also handle POST, PUT, DELETE for full API support
    r.post("/:path", [this](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        co_return co_await proxy_request_handler(ctx, impl_->request_handler);
    });

    r.put("/:path", [this](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        co_return co_await proxy_request_handler(ctx, impl_->request_handler);
    });

    r.del("/:path", [this](elio::http::context& ctx)
           -> elio::coro::task<elio::http::response> {
        co_return co_await proxy_request_handler(ctx, impl_->request_handler);
    });

    // Create HTTP server with router
    impl_->http_server = std::make_unique<elio::http::server>(std::move(r));

    // Set not found handler
    impl_->http_server->set_not_found_handler([](elio::http::context& ctx)
        -> elio::coro::task<elio::http::response> {
        std::string json = R"({"error": "Not Found", "path": ")" +
                          std::string(ctx.req().path()) + R"("})";
        co_return elio::http::response(elio::http::status::not_found, json,
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
        Logger::instance().info("Proxy HTTP server thread started");

        elio::run([this, bind_addr, opts]() -> elio::coro::task<void> {
            auto listen_task = impl_->http_server->listen(bind_addr, opts);
            auto listen_handle = std::move(listen_task).spawn();

            // Wait for running flag to become false
            while (impl_->running) {
                co_await elio::time::sleep_for(std::chrono::milliseconds(100));
            }

            // Stop the HTTP server
            Logger::instance().info("Stopping HTTP server");
            impl_->http_server->stop();
            // Wait for listen task to complete
            co_await listen_handle;

            Logger::instance().info("HTTP server stopped");
            co_return;
        }());

        Logger::instance().info("Proxy HTTP server thread exiting");
    });

    // Give the server thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Logger::instance().info("Proxy server started successfully");
    return true;
}

void ProxyServer::stop() {
    if (impl_->running) {
        Logger::instance().info("Stopping proxy server - setting running to false");
        impl_->running = false;

        // Wait for server thread to finish briefly
        if (impl_->server_thread.joinable()) {
            constexpr auto timeout = std::chrono::seconds(1);
            auto start = std::chrono::steady_clock::now();

            while (impl_->server_thread.joinable()) {
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= timeout) {
                    Logger::instance().warning("Server thread join timeout, detaching");
                    impl_->server_thread.detach();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        Logger::instance().info("Proxy server stopped");
    }
}

ProxyMetrics ProxyServer::get_metrics() const {
    std::lock_guard<std::mutex> lock(impl_->metrics_mutex);
    return impl_->metrics;
}

bool ProxyServer::is_running() const {
    return impl_->running.load();
}

void ProxyServer::set_transfer_manager(std::shared_ptr<TransferManager> transfer_manager) {
    impl_->transfer_manager = transfer_manager;
    if (impl_->request_handler) {
        impl_->request_handler->set_transfer_manager(transfer_manager);
    }
    Logger::instance().info("Transfer manager connected to proxy server");
}

void ProxyServer::set_transfer_manager(std::unique_ptr<TransferManager> transfer_manager) {
    impl_->transfer_manager = std::move(transfer_manager);
    if (impl_->request_handler) {
        impl_->request_handler->set_transfer_manager(impl_->transfer_manager);
    }
    Logger::instance().info("Transfer manager connected to proxy server");
}

void ProxyServer::set_p2p_fallback_enabled(bool enabled) {
    impl_->enable_p2p_fallback = enabled;
    if (impl_->request_handler) {
        impl_->request_handler->set_p2p_fallback_enabled(enabled);
    }
    Logger::instance().info("P2P fallback " + std::string(enabled ? "enabled" : "disabled"));
}

int ProxyServer::get_stop_event_fd() const {
    return impl_->stop_pipe[0];
}

} // namespace eliop2p
