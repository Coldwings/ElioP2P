#ifndef ELIOP2P_STORAGE_OSS_CLIENT_H
#define ELIOP2P_STORAGE_OSS_CLIENT_H

#include "eliop2p/storage/s3_client.h"
#include <string>

namespace eliop2p {

// Alibaba Cloud OSS client
// OSS is S3-compatible, so we inherit from S3Client
// but handle OSS-specific features and signing

class OSSClient : public S3Client {
public:
    OSSClient(const StorageConfig& config);
    ~OSSClient() override;

    // Override to return OSS type
    StorageBackend get_backend_type() const override { return StorageBackend::OSS; }

    // OSS-specific: Get object URL (without signature)
    std::string get_object_url(const std::string& bucket, const std::string& key) const;

    // OSS-specific: Generate custom signed URL
    std::string generate_signed_url(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds = 3600);

    // OSS-specific: Generate presigned URL (override)
    std::string generate_presigned_url(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds = 3600) override;

    // OSS-specific: Set bucket lifecycle rule
    bool set_bucket_lifecycle(
        const std::string& bucket,
        const std::string& rule_id,
        int days_after_expiration);

    // OSS-specific: Enable bucket CORS
    bool enable_bucket_cors(
        const std::string& bucket,
        const std::vector<std::string>& allowed_origins,
        const std::vector<std::string>& allowed_methods);

    // OSS-specific: Authenticate request
    using S3Client::authenticate_request;
    AuthenticatedRequest authenticate_request_oss(
        const std::string& method,
        const std::string& bucket,
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& original_headers,
        const std::string& original_query_string);

protected:
    // OSS-specific signing
    std::vector<std::pair<std::string, std::string>> sign_request_oss(
        const std::string& method,
        const std::string& bucket,
        const std::string& key,
        const std::string& query_string,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& payload_hash);

    std::string generate_presigned_url_internal_oss(
        const std::string& bucket,
        const std::string& key,
        uint64_t expires_in_seconds,
        const std::string& query_params);

private:
    struct OSSImpl;
    std::unique_ptr<OSSImpl> oss_impl_;
};

} // namespace eliop2p

#endif // ELIOP2P_STORAGE_OSS_CLIENT_H
