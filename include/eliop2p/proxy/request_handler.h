#ifndef ELIOP2P_PROXY_REQUEST_HANDLER_H
#define ELIOP2P_PROXY_REQUEST_HANDLER_H

#include "eliop2p/base/config.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/p2p/transfer.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <elio/elio.hpp>

namespace eliop2p {

// HTTP request representation (simplified)
struct HttpRequest {
    std::string method;           // GET, PUT, POST, DELETE
    std::string path;
    std::string query_string;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    std::string get_header(const std::string& name) const;
    bool has_header(const std::string& name) const;
};

// HTTP response representation (simplified)
struct HttpResponse {
    uint16_t status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    void set_header(const std::string& name, const std::string& value);
    std::string get_header(const std::string& name) const;
};

// Authentication type detected from request
enum class RequestAuthType {
    None,              // No authentication (public bucket)
    PresignedURL,      // Query string signature
    HeaderSigned,      // Header signature (x-amz-date)
    NonReusableHeader  // Header signature that cannot be reused
};

class RequestHandler {
public:
    RequestHandler(std::shared_ptr<ChunkManager> cache_manager,
                   std::shared_ptr<StorageClient> storage_client);
    ~RequestHandler();

    // Set transfer manager for P2P fallback
    void set_transfer_manager(std::shared_ptr<TransferManager> transfer_manager);

    // Enable/disable P2P fallback
    void set_p2p_fallback_enabled(bool enabled);

    // Restrict serving to a single bucket (empty = no restriction).
    // Call during startup, before requests arrive.
    void set_allowed_bucket(std::string bucket);

    // Handle incoming HTTP request
    // Returns response - caller is responsible for sending it
    elio::coro::task<HttpResponse> handle_request(const HttpRequest& request);

    // Parse cache key from request
    // Extracts bucket, object key, and authentication info
    struct CacheKeyInfo {
        std::string bucket;
        std::string object_key;
        std::string full_object_key;  // bucket/key
        std::optional<std::string> presigned_url;
        std::optional<std::string> auth_header;
        std::optional<std::string> etag;
        std::string query_string;
        RequestAuthType auth_type = RequestAuthType::None;
    };

    std::optional<CacheKeyInfo> parse_cache_key(const HttpRequest& request);

    // Check if request can be served from cache
    bool is_cacheable(const HttpRequest& request) const;

    // Get cache key for request
    std::string get_cache_key(const CacheKeyInfo& info) const;

    // Detect authentication type from request
    RequestAuthType detect_auth_type(const HttpRequest& request) const;

    // ---- Chunked object access -------------------------------------------
    // Objects are split into fixed-size chunks (config chunk_size_mb) keyed
    // as "<bucket>/<key>_<index>". Each chunk is cached, announced, and
    // fetched independently, so large objects can be served partially
    // (HTTP Range) and distributed chunk-by-chunk over P2P.

    struct ObjectInfo {
        uint64_t size = 0;
        std::string etag;
        // Per-chunk SHA256 (hex), learned progressively as chunks are
        // fetched from the storage origin. Acts as the trust anchor for
        // verifying P2P-downloaded chunk content.
        std::vector<std::string> chunk_hashes;
    };

    // Byte range parsed from an HTTP Range header (inclusive start/end).
    struct ByteRange {
        uint64_t start = 0;
        uint64_t end = 0;  // inclusive
        bool valid = false;
    };

    // Parse "bytes=a-b", "bytes=a-", "bytes=-b" against the object size.
    static ByteRange parse_range_header(const std::string& header, uint64_t object_size);

    // Resolve object size/etag: cached meta chunk first, HEAD request to
    // storage otherwise (the result is then cached as a meta chunk).
    elio::coro::task<std::optional<ObjectInfo>>
    get_object_info(const CacheKeyInfo& info);

    // Fetch a single chunk by index: local cache -> P2P -> storage range GET.
    // Returns the chunk data; nullopt if every source failed.
    elio::coro::task<std::shared_ptr<const std::vector<uint8_t>>>
    fetch_chunk(const CacheKeyInfo& info, const ObjectInfo& obj, uint64_t chunk_index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_PROXY_REQUEST_HANDLER_H
