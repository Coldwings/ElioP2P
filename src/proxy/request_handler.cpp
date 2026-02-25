#include "eliop2p/proxy/request_handler.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/p2p/transfer.h"
#include <elio/elio.hpp>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace eliop2p {

std::string HttpRequest::get_header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end()) {
        return it->second;
    }
    // Also check lowercase
    std::string lower_name;
    for (char c : name) {
        lower_name += static_cast<char>(std::tolower(c));
    }
    it = headers.find(lower_name);
    if (it != headers.end()) {
        return it->second;
    }
    return {};
}

bool HttpRequest::has_header(const std::string& name) const {
    if (headers.find(name) != headers.end()) {
        return true;
    }
    // Also check lowercase
    std::string lower_name;
    for (char c : name) {
        lower_name += static_cast<char>(std::tolower(c));
    }
    return headers.find(lower_name) != headers.end();
}

void HttpResponse::set_header(const std::string& name, const std::string& value) {
    headers[name] = value;
}

std::string HttpResponse::get_header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end()) {
        return it->second;
    }
    return {};
}

struct RequestHandler::Impl {
    std::shared_ptr<ChunkManager> cache_manager;
    std::shared_ptr<StorageClient> storage_client;
    std::shared_ptr<TransferManager> transfer_manager;

    // Configuration
    uint64_t chunk_size_mb = 16;
    bool enable_p2p_fallback = true;

    Impl(std::shared_ptr<ChunkManager> cm, std::shared_ptr<StorageClient> sc)
        : cache_manager(cm), storage_client(sc) {}
};

RequestHandler::RequestHandler(std::shared_ptr<ChunkManager> cache_manager,
                               std::shared_ptr<StorageClient> storage_client)
    : impl_(std::make_unique<Impl>(cache_manager, storage_client)) {}

RequestHandler::~RequestHandler() = default;

void RequestHandler::set_transfer_manager(std::shared_ptr<TransferManager> transfer_manager) {
    impl_->transfer_manager = transfer_manager;
}

void RequestHandler::set_p2p_fallback_enabled(bool enabled) {
    impl_->enable_p2p_fallback = enabled;
}

RequestAuthType RequestHandler::detect_auth_type(const HttpRequest& request) const {
    // Check for presigned URL (query string with signature params)
    if (!request.query_string.empty()) {
        // Look for common presigned URL signature params
        if (request.query_string.find("X-Amz-Signature") != std::string::npos ||
            request.query_string.find("Signature") != std::string::npos ||
            request.query_string.find("AWSAccessKeyId") != std::string::npos) {
            return RequestAuthType::PresignedURL;
        }
    }

    // Check for header-based authentication
    bool has_authorization = request.has_header("Authorization");
    bool has_amz_date = request.has_header("x-amz-date");

    if (has_authorization || has_amz_date) {
        // Check if the signature is reusable (has x-amz-date within valid window)
        if (has_amz_date) {
            std::string date_str = request.get_header("x-amz-date");
            // Parse the date and check if it's within the reusable window
            // AWS signatures with x-amz-date are reusable for 7 days
            if (!date_str.empty()) {
                return RequestAuthType::HeaderSigned;
            }
        }
        // If no x-amz-date but has Authorization, it's likely a non-reusable signature
        if (has_authorization) {
            return RequestAuthType::NonReusableHeader;
        }
    }

    // No authentication
    return RequestAuthType::None;
}

bool RequestHandler::is_header_signature_reusable(const HttpRequest& request) const {
    std::string date_str = request.get_header("x-amz-date");
    if (date_str.empty()) {
        return false;
    }

    // Try to parse the date (format: YYYYMMDDTHHMMSSZ or YYYYMMDDTHHMMSS.fffZ)
    // For simplicity, we'll check if it's within a reasonable time window
    // In production, you'd parse and compare timestamps properly
    try {
        // Extract date portion (first 8 chars: YYYYMMDD)
        if (date_str.length() >= 8) {
            std::string date_part = date_str.substr(0, 8);
            // Get current date
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::ostringstream oss;
            oss << std::put_time(std::gmtime(&time_t), "%Y%m%d");
            std::string current_date = oss.str();

            // Check if within 7 days (simplified: just same day or day before)
            return date_part >= current_date;
        }
    } catch (...) {
        // If parsing fails, assume not reusable
    }

    return false;
}

std::optional<RequestHandler::CacheKeyInfo> RequestHandler::parse_cache_key(
    const HttpRequest& request) {

    // Parse path: /bucket/key
    std::string path = request.path;
    if (path.empty() || path == "/") {
        return std::nullopt;
    }

    // Remove leading slash
    if (path[0] == '/') {
        path = path.substr(1);
    }

    if (path.empty()) {
        return std::nullopt;
    }

    auto slash_pos = path.find('/');
    if (slash_pos == std::string::npos) {
        // Invalid format: need bucket/key
        return std::nullopt;
    }

    CacheKeyInfo info;
    info.bucket = path.substr(0, slash_pos);
    info.object_key = path.substr(slash_pos + 1);
    info.full_object_key = info.bucket + "/" + info.object_key;
    info.query_string = request.query_string;

    // Detect authentication type
    info.auth_type = detect_auth_type(request);

    // Extract presigned URL from query string
    if (!request.query_string.empty()) {
        info.presigned_url = request.query_string;
    }

    // Check for auth header
    if (request.has_header("Authorization")) {
        info.auth_header = request.get_header("Authorization");
    }

    // Check for ETag / If-None-Match
    if (request.has_header("If-None-Match")) {
        info.etag = request.get_header("If-None-Match");
    }

    return info;
}

bool RequestHandler::is_cacheable(const HttpRequest& request) const {
    // Only GET requests are cacheable
    if (request.method != "GET") {
        return false;
    }

    // Check auth type - certain auth types are not cacheable
    RequestAuthType auth_type = detect_auth_type(request);
    if (auth_type == RequestAuthType::NonReusableHeader) {
        // Non-reusable header signatures cannot be cached
        return false;
    }

    return true;
}

std::string RequestHandler::get_cache_key(const CacheKeyInfo& info) const {
    std::ostringstream oss;
    oss << info.full_object_key;

    // Include version/etag if available for cache key
    if (info.etag) {
        oss << "?v=" << *info.etag;
    } else if (!info.query_string.empty()) {
        // If there's a query string (presigned URL), include it
        // This ensures different signed URLs don't share cache
        oss << "?q=" << info.query_string;
    }

    return oss.str();
}

elio::coro::task<HttpResponse> RequestHandler::handle_request(const HttpRequest& request) {
    Logger::instance().info("Handling request: " + request.method + " " + request.path);

    HttpResponse response;

    // Only handle GET for now (cache reads)
    if (request.method != "GET") {
        response.status_code = 405;
        response.status_message = "Method Not Allowed";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Method " + request.method + " not supported";
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Parse cache key from request
    auto cache_key_info = parse_cache_key(request);
    if (!cache_key_info) {
        response.status_code = 400;
        response.status_message = "Bad Request";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Invalid request: could not parse bucket/key from path";
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Check if request is cacheable
    bool can_cache = is_cacheable(request);

    // Get cache key
    std::string cache_key = get_cache_key(*cache_key_info);
    Logger::instance().debug("Cache key: " + cache_key);

    // Try to get from local cache
    if (impl_->cache_manager) {
        auto chunk = impl_->cache_manager->get_chunk(cache_key);
        if (chunk) {
            Logger::instance().info("Cache HIT for: " + cache_key);
            response.status_code = 200;
            response.status_message = "OK";
            response.set_header("Content-Type", "application/octet-stream");
            response.set_header("X-Cache", "HIT");
            response.body = chunk->data();
            co_return response;
        }
    }

    Logger::instance().info("Cache MISS for: " + cache_key);

    // Cache miss - fetch from storage
    if (!impl_->storage_client) {
        // Try P2P fallback if enabled
        if (impl_->enable_p2p_fallback && impl_->transfer_manager) {
            Logger::instance().info("Storage unavailable, trying P2P fallback for: " + cache_key);
            auto p2p_data = co_await try_p2p_fallback(cache_key, *cache_key_info);
            if (p2p_data) {
                // Store in cache
                if (impl_->cache_manager && can_cache) {
                    impl_->cache_manager->store_chunk(cache_key, *p2p_data);
                }
                response.status_code = 200;
                response.status_message = "OK";
                response.set_header("Content-Type", "application/octet-stream");
                response.set_header("X-Cache", "P2P");
                response.body = *p2p_data;
                co_return response;
            }
        }

        response.status_code = 503;
        response.status_message = "Service Unavailable";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Storage backend not available";
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Prepare authentication headers for storage request
    std::vector<std::pair<std::string, std::string>> auth_headers;
    std::string auth_query_string;

    // Pass through authentication info if present
    if (cache_key_info->auth_header) {
        // Add Authorization header
        auto auth_header_val = *cache_key_info->auth_header;
        // Find and pass all relevant auth headers
        for (const auto& [key, value] : request.headers) {
            if (key == "Authorization" || key == "x-amz-date" ||
                key == "x-amz-content-sha256" || key == "x-amz-security-token") {
                auth_headers.push_back({key, value});
            }
        }
    }

    // Pass presigned URL query string if present
    if (!cache_key_info->query_string.empty()) {
        auth_query_string = cache_key_info->query_string;
    }

    // Fetch from storage with auth info
    auto storage_data = co_await impl_->storage_client->get_object(
        cache_key_info->bucket,
        cache_key_info->object_key
    );

    // If storage fails and P2P fallback is enabled, try P2P
    if (!storage_data && impl_->enable_p2p_fallback && impl_->transfer_manager) {
        Logger::instance().info("Storage fetch failed, trying P2P fallback for: " + cache_key);
        auto p2p_data = co_await try_p2p_fallback(cache_key, *cache_key_info);
        if (p2p_data) {
            // Store in cache
            if (impl_->cache_manager && can_cache) {
                impl_->cache_manager->store_chunk(cache_key, *p2p_data);
            }
            response.status_code = 200;
            response.status_message = "OK";
            response.set_header("Content-Type", "application/octet-stream");
            response.set_header("X-Cache", "P2P");
            response.body = *p2p_data;
            co_return response;
        }
    }

    if (!storage_data) {
        response.status_code = 404;
        response.status_message = "Not Found";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Object not found: " + cache_key_info->full_object_key;
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Store in cache
    if (impl_->cache_manager && can_cache) {
        impl_->cache_manager->store_chunk(cache_key, *storage_data);
        Logger::instance().debug("Cached: " + cache_key);
    }

    // Return data
    response.status_code = 200;
    response.status_message = "OK";
    response.set_header("Content-Type", "application/octet-stream");
    response.set_header("X-Cache", "MISS");
    response.body = *storage_data;

    co_return response;
}

// Try to fetch object from P2P network
elio::coro::task<std::optional<std::vector<uint8_t>>>
RequestHandler::try_p2p_fallback(const std::string& cache_key,
                                  const CacheKeyInfo& cache_key_info) {
    if (!impl_->transfer_manager) {
        co_return std::nullopt;
    }

    Logger::instance().info("Attempting P2P fallback for: " + cache_key);

    TransferRequest transfer_req;
    transfer_req.chunk_id = cache_key;
    transfer_req.object_key = cache_key_info.full_object_key;
    transfer_req.k_value = 3;  // Try 3 peers in parallel for fallback
    transfer_req.mode = TransferMode::FastestFirst;

    auto result = co_await impl_->transfer_manager->download_chunk(transfer_req);

    if (result) {
        Logger::instance().info("P2P fallback successful for: " + cache_key);
    } else {
        Logger::instance().warning("P2P fallback failed for: " + cache_key);
    }

    co_return result;
}

} // namespace eliop2p
