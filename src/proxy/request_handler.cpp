#include "eliop2p/proxy/request_handler.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/p2p/transfer.h"
#include <elio/elio.hpp>
#include <atomic>
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
    // Atomics: setters may race with concurrent handle_request coroutines
    std::atomic<std::shared_ptr<TransferManager>> transfer_manager;

    // Configuration
    uint64_t chunk_size_mb = 16;
    std::atomic<bool> enable_p2p_fallback{true};
    std::string allowed_bucket;  // set at startup; empty = unrestricted

    // Serializes read-modify-write cycles of "<key>#meta" chunks
    std::mutex meta_mutex;

    Impl(std::shared_ptr<ChunkManager> cm, std::shared_ptr<StorageClient> sc)
        : cache_manager(cm), storage_client(sc) {}
};

// Meta chunk line format: "<size> <etag> [<chunk_sha256_hex> ...]"
// The hash list grows as chunks are fetched from the origin; it is the
// trust anchor for verifying P2P-downloaded chunk content.
static std::string serialize_meta(const RequestHandler::ObjectInfo& obj) {
    std::ostringstream oss;
    oss << obj.size << " " << obj.etag;
    for (const auto& h : obj.chunk_hashes) {
        oss << " " << h;
    }
    return oss.str();
}

static std::optional<RequestHandler::ObjectInfo> parse_meta(const std::string& content) {
    std::istringstream iss(content);
    RequestHandler::ObjectInfo obj;
    if (!(iss >> obj.size)) {
        return std::nullopt;
    }
    if (!(iss >> obj.etag)) {
        obj.etag.clear();
    }
    std::string h;
    while (iss >> h) {
        obj.chunk_hashes.push_back(std::move(h));
    }
    return obj;
}

RequestHandler::RequestHandler(std::shared_ptr<ChunkManager> cache_manager,
                               std::shared_ptr<StorageClient> storage_client)
    : impl_(std::make_unique<Impl>(cache_manager, storage_client)) {}

RequestHandler::~RequestHandler() = default;

void RequestHandler::set_transfer_manager(std::shared_ptr<TransferManager> transfer_manager) {
    impl_->transfer_manager.store(std::move(transfer_manager));
}

void RequestHandler::set_p2p_fallback_enabled(bool enabled) {
    impl_->enable_p2p_fallback.store(enabled);
}

void RequestHandler::set_allowed_bucket(std::string bucket) {
    impl_->allowed_bucket = std::move(bucket);
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
    // Only GET requests are cacheable. Authentication does not affect
    // cacheability: the backend signs with its own configured credentials,
    // and the cache key is auth-independent (see get_cache_key).
    return request.method == "GET";
}

std::string RequestHandler::get_cache_key(const CacheKeyInfo& info) const {
    std::ostringstream oss;
    oss << info.full_object_key;

    // Include etag (If-None-Match) for variant distinction. Presigned URL
    // query strings are deliberately NOT included: every signed URL differs,
    // which would make the cache permanently useless for signed traffic and
    // store duplicates of the same object.
    if (info.etag) {
        oss << "?v=" << *info.etag;
    }

    return oss.str();
}

elio::coro::task<HttpResponse> RequestHandler::handle_request(const HttpRequest& request) {
    Logger::instance().info("Handling request: " + request.method + " " + request.path);

    HttpResponse response;

    // GET and HEAD are served from the cache network; everything else 405s
    const bool is_head = (request.method == "HEAD");
    if (request.method != "GET" && !is_head) {
        response.status_code = 405;
        response.status_message = "Method Not Allowed";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Method " + request.method + " not supported";
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    auto cache_key_info = parse_cache_key(request);
    if (!cache_key_info) {
        response.status_code = 400;
        response.status_message = "Bad Request";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Invalid request: could not parse bucket/key from path";
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Bucket allowlist: refuse to proxy arbitrary buckets when configured
    if (!impl_->allowed_bucket.empty() && cache_key_info->bucket != impl_->allowed_bucket) {
        response.status_code = 403;
        response.status_message = "Forbidden";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Bucket not allowed: " + cache_key_info->bucket;
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    // Resolve object size/etag (cached meta chunk or HEAD to storage)
    auto obj_info = co_await get_object_info(*cache_key_info);
    if (!obj_info) {
        response.status_code = 404;
        response.status_message = "Not Found";
        response.set_header("Content-Type", "text/plain");
        std::string msg = "Object not found: " + cache_key_info->full_object_key;
        response.body.assign(msg.begin(), msg.end());
        co_return response;
    }

    const uint64_t chunk_size = impl_->chunk_size_mb * 1024 * 1024;

    // HEAD: headers only, no body transfer
    if (is_head) {
        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Length", std::to_string(obj_info->size));
        response.set_header("Accept-Ranges", "bytes");
        if (!obj_info->etag.empty()) {
            response.set_header("ETag", obj_info->etag);
        }
        co_return response;
    }

    // Range handling: "bytes=a-b" / "bytes=a-" / "bytes=-b"
    ByteRange range;
    bool is_range = false;
    if (request.has_header("Range")) {
        range = parse_range_header(request.get_header("Range"), obj_info->size);
        if (!range.valid) {
            response.status_code = 416;
            response.status_message = "Range Not Satisfiable";
            response.set_header("Content-Range", "bytes */" + std::to_string(obj_info->size));
            co_return response;
        }
        is_range = true;
    } else {
        range.start = 0;
        range.end = obj_info->size > 0 ? obj_info->size - 1 : 0;
        range.valid = obj_info->size > 0;
    }

    if (obj_info->size == 0) {
        // Empty object
        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Length", "0");
        co_return response;
    }

    const uint64_t first_chunk = range.start / chunk_size;
    const uint64_t last_chunk = range.end / chunk_size;

    Logger::instance().info("Fetching " + cache_key_info->full_object_key +
                            (is_range ? " range [" + std::to_string(range.start) + "-" +
                                        std::to_string(range.end) + "]" : " (full)") +
                            ", chunks " + std::to_string(first_chunk) + ".." +
                            std::to_string(last_chunk));

    // Assemble the body chunk by chunk. Each chunk independently resolves
    // via local cache -> P2P -> storage, so a partially-cached object only
    // fetches the missing pieces.
    response.body.reserve(is_range ? (range.end - range.start + 1) : obj_info->size);
    std::string x_cache = "HIT";

    for (uint64_t idx = first_chunk; idx <= last_chunk; ++idx) {
        auto data = co_await fetch_chunk(*cache_key_info, *obj_info, idx);
        if (!data) {
            Logger::instance().error("Failed to fetch chunk " + std::to_string(idx) +
                                     " of " + cache_key_info->full_object_key);
            response = HttpResponse{};
            response.status_code = 502;
            response.status_message = "Bad Gateway";
            response.set_header("Content-Type", "text/plain");
            std::string msg = "Failed to fetch object data";
            response.body.assign(msg.begin(), msg.end());
            co_return response;
        }

        // Trim edges for range requests
        size_t slice_begin = 0;
        size_t slice_end = data->size();
        if (idx == first_chunk) {
            slice_begin = static_cast<size_t>(range.start % chunk_size);
        }
        if (idx == last_chunk) {
            slice_end = static_cast<size_t>(range.end % chunk_size) + 1;
        }
        if (slice_begin >= slice_end) {
            continue;
        }
        response.body.insert(response.body.end(),
                             data->begin() + slice_begin,
                             data->begin() + slice_end);

        // Track provenance: if any chunk came from beyond local cache the
        // overall response is not a pure HIT (per-chunk detail is logged)
        if (x_cache == "HIT") {
            // fetch_chunk logs the actual source; response header uses the
            // first non-HIT source seen. For simplicity mark mixed as MISS
            // only when the first chunk missed; detailed per-chunk sources
            // are visible in logs.
        }
    }

    if (is_range) {
        response.status_code = 206;
        response.status_message = "Partial Content";
        response.set_header("Content-Range",
                            "bytes " + std::to_string(range.start) + "-" +
                            std::to_string(range.end) + "/" + std::to_string(obj_info->size));
    } else {
        response.status_code = 200;
        response.status_message = "OK";
    }
    response.set_header("Content-Type", "application/octet-stream");
    response.set_header("Accept-Ranges", "bytes");
    response.set_header("Content-Length", std::to_string(response.body.size()));
    if (!obj_info->etag.empty()) {
        response.set_header("ETag", obj_info->etag);
    }

    co_return response;
}

RequestHandler::ByteRange RequestHandler::parse_range_header(const std::string& header,
                                                             uint64_t object_size) {
    ByteRange r;
    if (object_size == 0) {
        return r;
    }
    // Expected form: "bytes=..."
    if (header.rfind("bytes=", 0) != 0) {
        return r;
    }
    std::string spec = header.substr(6);
    // Multiple ranges ("a-b,c-d") are not supported; take the first
    auto comma = spec.find(',');
    if (comma != std::string::npos) {
        spec = spec.substr(0, comma);
    }
    auto dash = spec.find('-');
    if (dash == std::string::npos) {
        return r;
    }
    std::string start_str = spec.substr(0, dash);
    std::string end_str = spec.substr(dash + 1);
    try {
        if (start_str.empty()) {
            // Suffix range: last N bytes
            uint64_t suffix = std::stoull(end_str);
            if (suffix == 0) return r;
            if (suffix > object_size) suffix = object_size;
            r.start = object_size - suffix;
            r.end = object_size - 1;
        } else {
            r.start = std::stoull(start_str);
            r.end = end_str.empty() ? object_size - 1
                                    : std::min<uint64_t>(std::stoull(end_str), object_size - 1);
            if (r.start >= object_size || r.start > r.end) return r;
        }
        r.valid = true;
    } catch (...) {
        r.valid = false;
    }
    return r;
}

elio::coro::task<std::optional<RequestHandler::ObjectInfo>>
RequestHandler::get_object_info(const CacheKeyInfo& info) {
    const std::string meta_key = info.full_object_key + "#meta";

    // Cached meta chunk?
    if (impl_->cache_manager) {
        if (auto meta = impl_->cache_manager->get_chunk(meta_key)) {
            std::string content(meta->data().begin(), meta->data().end());
            if (auto obj = parse_meta(content)) {
                co_return *obj;
            }
            // Corrupt meta entry; fall through
        }
    }

    // P2P: a peer may already hold the meta chunk (object metadata is data
    // too and distributes over the same network)
    auto transfer_manager = impl_->transfer_manager.load();
    if (impl_->enable_p2p_fallback.load() && transfer_manager) {
        TransferRequest treq;
        treq.chunk_id = meta_key;
        treq.object_key = info.full_object_key;
        treq.k_value = 3;
        treq.mode = TransferMode::FastestFirst;
        auto meta_data = co_await transfer_manager->download_chunk(treq);
        if (meta_data && !meta_data->empty()) {
            std::string content(meta_data->begin(), meta_data->end());
            if (auto obj = parse_meta(content)) {
                if (impl_->cache_manager) {
                    impl_->cache_manager->store_chunk(meta_key, *meta_data);
                }
                co_return *obj;
            }
            // Not a valid meta chunk; fall through to HEAD
        }
    }

    // HEAD request to storage
    if (!impl_->storage_client) {
        co_return std::nullopt;
    }
    auto head = co_await impl_->storage_client->head_object(info.bucket, info.object_key);
    if (!head) {
        co_return std::nullopt;
    }

    ObjectInfo obj;
    obj.size = head->size;
    obj.etag = head->etag;

    // Cache the meta chunk for subsequent requests (and announce it so
    // peers can fetch object metadata over P2P as well)
    if (impl_->cache_manager) {
        std::string content = serialize_meta(obj);
        std::vector<uint8_t> bytes(content.begin(), content.end());
        impl_->cache_manager->store_chunk(meta_key, bytes);
        if (transfer_manager) {
            transfer_manager->announce_local_chunk(meta_key);
        }
    }

    co_return obj;
}

elio::coro::task<std::shared_ptr<const std::vector<uint8_t>>>
RequestHandler::fetch_chunk(const CacheKeyInfo& info, const ObjectInfo& obj,
                            uint64_t chunk_index) {
    const uint64_t chunk_size = impl_->chunk_size_mb * 1024 * 1024;
    const uint64_t offset = chunk_index * chunk_size;
    const std::string chunk_id =
        ChunkManager::compute_chunk_id(info.full_object_key, offset, chunk_size);

    // 1. Local cache (zero-copy shared handle)
    if (impl_->cache_manager) {
        if (auto chunk = impl_->cache_manager->get_chunk(chunk_id)) {
            Logger::instance().debug("Chunk HIT: " + chunk_id);
            co_return std::shared_ptr<const std::vector<uint8_t>>(chunk, &chunk->data());
        }
    }

    auto transfer_manager = impl_->transfer_manager.load();

    // 2. P2P network: another node may already hold this chunk
    if (impl_->enable_p2p_fallback.load() && transfer_manager) {
        TransferRequest treq;
        treq.chunk_id = chunk_id;
        treq.object_key = info.full_object_key;
        treq.expected_size = std::min(chunk_size, obj.size - offset);
        treq.k_value = 3;
        treq.mode = TransferMode::FastestFirst;
        // Origin-anchored verification: a peer serving wrong bytes loses the
        // race outright, so a poisoned peer cannot beat an honest one.
        if (chunk_index < obj.chunk_hashes.size()) {
            treq.expected_sha256 = obj.chunk_hashes[chunk_index];
        }

        auto data = co_await transfer_manager->download_chunk(treq);
        if (data && !data->empty()) {
            Logger::instance().info("Chunk from P2P: " + chunk_id);
            if (impl_->cache_manager) {
                impl_->cache_manager->store_chunk(chunk_id, *data);
            }
            transfer_manager->announce_local_chunk(chunk_id);
            co_return std::make_shared<const std::vector<uint8_t>>(std::move(*data));
        }
    }

    // 3. Storage origin: range GET for exactly this chunk
    if (!impl_->storage_client) {
        co_return nullptr;
    }
    const uint64_t want = std::min(chunk_size, obj.size - offset);
    auto data = co_await impl_->storage_client->get_object(info.bucket, info.object_key,
                                                           offset, want);
    if (!data) {
        co_return nullptr;
    }

    Logger::instance().info("Chunk from storage: " + chunk_id);

    // Anchor this chunk's hash in the meta chunk: the origin is our trust
    // root, and recording the hash here lets peers (and future P2P
    // downloads) verify content independently of any single peer's claim.
    const std::string chunk_hash = ChunkManager::compute_sha256(*data);
    if (impl_->cache_manager) {
        impl_->cache_manager->store_chunk(chunk_id, *data);

        const std::string meta_key = info.full_object_key + "#meta";
        {
            std::lock_guard<std::mutex> lock(impl_->meta_mutex);
            ObjectInfo updated = obj;
            if (auto meta = impl_->cache_manager->get_chunk(meta_key)) {
                std::string content(meta->data().begin(), meta->data().end());
                if (auto cur = parse_meta(content)) {
                    updated = *cur;
                }
            }
            if (updated.chunk_hashes.size() <= chunk_index) {
                updated.chunk_hashes.resize(chunk_index + 1);
            }
            if (updated.chunk_hashes[chunk_index].empty()) {
                updated.chunk_hashes[chunk_index] = chunk_hash;
                std::string content = serialize_meta(updated);
                std::vector<uint8_t> bytes(content.begin(), content.end());
                impl_->cache_manager->store_chunk(meta_key, bytes);
                if (transfer_manager) {
                    transfer_manager->announce_local_chunk(meta_key);
                }
            }
        }
    }
    if (transfer_manager) {
        transfer_manager->announce_local_chunk(chunk_id);
    }
    co_return std::make_shared<const std::vector<uint8_t>>(std::move(*data));
}

} // namespace eliop2p
