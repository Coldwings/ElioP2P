#ifndef ELIOP2P_STORAGE_S3_CLIENT_H
#define ELIOP2P_STORAGE_S3_CLIENT_H

#include "eliop2p/base/config.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>
#include <functional>
#include <elio/elio.hpp>
#include <elio/http/http_message.hpp>

namespace elio::runtime {
class scheduler;
}

namespace eliop2p {

// Storage backend type
enum class StorageBackend {
    S3,        // AWS S3, MinIO, etc.
    OSS        // Alibaba Cloud OSS
};

// Authentication type
enum class AuthType {
    None,           // No authentication (public bucket)
    PresignedURL,   // Query string signature
    HeaderSigned    // Header signature (x-amz-date)
};

struct ObjectMetadata {
    std::string key;
    uint64_t size;
    std::string etag;
    std::optional<std::string> content_type;
    std::optional<std::string> last_modified;
    std::optional<std::string> version_id;
};

struct ListObjectsResult {
    std::vector<ObjectMetadata> objects;
    std::vector<std::string> common_prefixes;
    bool is_truncated = false;
    std::string continuation_token;
};

// Storage client interface for polymorphic access
class StorageClient {
public:
    virtual ~StorageClient() = default;

    // Test connection
    virtual bool test_connection() = 0;

    // List buckets
    virtual elio::coro::task<std::optional<std::vector<std::string>>> list_buckets() = 0;

    // List objects in bucket
    virtual elio::coro::task<std::optional<ListObjectsResult>> list_objects(
        const std::string& bucket,
        const std::string& prefix = "",
        const std::string& continuation_token = "",
        uint32_t max_keys = 1000) = 0;

    // Get object metadata (head object)
    virtual elio::coro::task<std::optional<ObjectMetadata>> head_object(
        const std::string& bucket,
        const std::string& key) = 0;

    // Get object (download)
    virtual elio::coro::task<std::optional<std::vector<uint8_t>>> get_object(
        const std::string& bucket,
        const std::string& key,
        uint64_t offset = 0,
        uint64_t length = 0) = 0;

    // Put object (upload)
    virtual elio::coro::task<bool> put_object(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data,
        const std::string& content_type = "application/octet-stream") = 0;

    // Delete object
    virtual elio::coro::task<bool> delete_object(
        const std::string& bucket,
        const std::string& key) = 0;

    // Generate presigned URL
    virtual std::string generate_presigned_url(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds = 3600) = 0;

    // Check if bucket exists
    virtual elio::coro::task<bool> bucket_exists(const std::string& bucket) = 0;

    // Get endpoint
    virtual const std::string& get_endpoint() const = 0;
    virtual const std::string& get_region() const = 0;

    // Get backend type
    virtual StorageBackend get_backend_type() const = 0;

    // Set scheduler for async HTTP operations (virtual for interface)
    virtual void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) = 0;
};

// Forward declaration
class OSSClient;

// S3-compatible client (AWS S3, MinIO, etc.)
class S3Client : public StorageClient {
public:
    // Public Impl for subclass access
    struct Impl {
        std::string access_key;
        std::string secret_key;
    };

    S3Client(const StorageConfig& config);
    ~S3Client() override;

    // Test connection
    bool test_connection() override;

    // List buckets
    elio::coro::task<std::optional<std::vector<std::string>>> list_buckets() override;

    // List objects in bucket
    elio::coro::task<std::optional<ListObjectsResult>> list_objects(
        const std::string& bucket,
        const std::string& prefix = "",
        const std::string& continuation_token = "",
        uint32_t max_keys = 1000) override;

    // Get object metadata (head object)
    elio::coro::task<std::optional<ObjectMetadata>> head_object(
        const std::string& bucket,
        const std::string& key) override;

    // Get object (download)
    elio::coro::task<std::optional<std::vector<uint8_t>>> get_object(
        const std::string& bucket,
        const std::string& key,
        uint64_t offset = 0,
        uint64_t length = 0) override;

    // Put object (upload)
    elio::coro::task<bool> put_object(
        const std::string& bucket,
        const std::string& key,
        const std::vector<uint8_t>& data,
        const std::string& content_type = "application/octet-stream") override;

    // Delete object
    elio::coro::task<bool> delete_object(
        const std::string& bucket,
        const std::string& key) override;

    // Generate presigned URL
    std::string generate_presigned_url(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds = 3600) override;

    // Check if bucket exists
    elio::coro::task<bool> bucket_exists(const std::string& bucket) override;

    // Get endpoint
    const std::string& get_endpoint() const override { return config_.endpoint; }
    const std::string& get_region() const override { return config_.region; }

    // Get backend type
    StorageBackend get_backend_type() const override { return StorageBackend::S3; }

    // Get config reference for subclasses
    const StorageConfig& get_config() const { return config_; }

    // Get impl pointer for subclasses
    Impl* get_impl() const { return impl_.get(); }

    // Set authentication credentials
    void set_credentials(const std::string& access_key, const std::string& secret_key);

    // Get current auth type
    AuthType get_auth_type() const { return auth_type_; }

    // Set auth type
    void set_auth_type(AuthType type) { auth_type_ = type; }

    // Set scheduler for async HTTP operations
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);

    // Process incoming request and forward to storage with auth
    // Returns the modified request that can be sent to storage
    struct AuthenticatedRequest {
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        bool needs_passthrough;  // true if auth cannot be handled, pass through
    };

    // Authenticate an incoming request and prepare for storage access
    // This is used by the proxy to handle pre-signed URLs and header-signed requests
    AuthenticatedRequest authenticate_request(
        const std::string& method,
        const std::string& bucket,
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& original_headers,
        const std::string& original_query_string);

protected:
    // Build URL for object operation
    std::string build_object_url(
        const std::string& bucket,
        const std::string& key,
        bool use_path_style = false) const;

    // Sign request with AWS Signature V4
    std::vector<std::pair<std::string, std::string>> sign_request(
        const std::string& method,
        const std::string& bucket,
        const std::string& key,
        const std::string& query_string,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& payload_hash);

    // Execute HTTP request with S3 signing
    elio::coro::task<std::optional<elio::http::response>> execute_request(
        const std::string& method,
        const std::string& bucket,
        const std::string& key,
        const std::string& query_string,
        const std::vector<std::pair<std::string, std::string>>& extra_headers,
        const std::string& body,
        const std::string& content_type);

    // Generate presigned URL with signature
    std::string generate_presigned_url_internal(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds,
        const std::string& query_params);

private:
    std::unique_ptr<Impl> impl_;
    StorageConfig config_;
    AuthType auth_type_ = AuthType::None;
    std::string access_key_;
    std::string secret_key_;
    std::shared_ptr<elio::runtime::scheduler> scheduler_;
};

// Storage client factory - auto-detects backend based on endpoint
class StorageClientFactory {
public:
    // Create client based on endpoint
    static std::unique_ptr<StorageClient> create(const StorageConfig& config);

    // Detect backend type from endpoint
    static StorageBackend detect_backend(const std::string& endpoint);

    // Detect from endpoint and credentials
    static StorageBackend detect_backend(const std::string& endpoint,
                                          const std::optional<std::string>& access_key);
};

} // namespace eliop2p

#endif // ELIOP2P_STORAGE_S3_CLIENT_H
