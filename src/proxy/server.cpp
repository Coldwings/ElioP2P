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
    std::shared_ptr<elio::runtime::scheduler> scheduler;
    std::thread server_thread;
    std::shared_ptr<TransferManager> transfer_manager;
    bool enable_p2p_fallback = true;

    // Server instance for elio::serve
    std::unique_ptr<elio::http::server> http_server;
};

ProxyServer::ProxyServer(const ProxyConfig& config,
                         std::shared_ptr<ChunkManager> cache_manager,
                         std::shared_ptr<StorageClient> storage_client)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->cache_manager = cache_manager;
    impl_->storage_client = storage_client;
    impl_->request_handler = std::make_shared<RequestHandler>(cache_manager, storage_client);

    // Create scheduler for HTTP server and storage client
    impl_->scheduler = std::make_shared<elio::runtime::scheduler>(4);
    impl_->scheduler->start();

    // Set scheduler on storage client if it's an S3 client
    if (storage_client) {
        // The storage client can use the scheduler for async HTTP operations
        // This is handled internally by the S3 client when making requests
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

    // Start HTTP server in a separate thread using elio::run
    impl_->server_thread = std::thread([this, bind_addr, opts]() {
        Logger::instance().info("Proxy HTTP server thread started");

        // Use elio::run to execute the HTTP server
        // This creates a scheduler, starts it, and runs the coroutine to completion
        auto server_task = [this, bind_addr, opts]() -> elio::coro::task<void> {
            co_await elio::serve(*impl_->http_server, impl_->http_server->listen(bind_addr, opts));
        };

        elio::run(server_task());

        Logger::instance().info("Proxy HTTP server thread exiting");
    });

    // Give the server thread time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Logger::instance().info("Proxy server started successfully");
    return true;
}

void ProxyServer::stop() {
    if (impl_->running) {
        Logger::instance().info("Stopping proxy server");
        impl_->running = false;

        // Stop the HTTP server
        if (impl_->http_server) {
            impl_->http_server->stop();
        }

        if (impl_->server_thread.joinable()) {
            impl_->server_thread.join();
        }

        // Shutdown scheduler
        if (impl_->scheduler) {
            impl_->scheduler->shutdown();
        }
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

} // namespace eliop2p
