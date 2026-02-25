#include "eliop2p/storage/oss_client.h"
#include "eliop2p/base/logger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <map>

namespace eliop2p {

// OSS-specific SHA1 for signature (OSS uses HMAC-SHA1)
static std::string hmac_sha1(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha1(), key.data(), key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash, &len);

    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// Get current UTC time in RFC 7231 format for OSS
static std::string get_current_time_oss() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    // Format: Mon, 02 Jan 2006 15:04:05 GMT
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

// URL encode for OSS query string
static std::string url_encode_oss(const std::string& value) {
    std::ostringstream oss;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return oss.str();
}

struct OSSClient::OSSImpl {
    // OSS-specific implementation data if needed
};

OSSClient::OSSClient(const StorageConfig& config)
    : S3Client(config), oss_impl_(std::make_unique<OSSImpl>()) {
    Logger::instance().info("OSSClient initialized with endpoint: " + config.endpoint);
}

OSSClient::~OSSClient() = default;

std::string OSSClient::get_object_url(
    const std::string& bucket,
    const std::string& key) const {

    std::string endpoint = get_endpoint();
    return "https://" + bucket + "." + endpoint + "/" + key;
}

std::string OSSClient::generate_signed_url(
    const std::string& bucket,
    const std::string& key,
    uint64_t expires_in_seconds) {

    Logger::instance().info("Generating OSS signed URL for: " + bucket + "/" + key);
    return generate_presigned_url_internal_oss(bucket, key, expires_in_seconds, "");
}

std::string OSSClient::generate_presigned_url(
    const std::string& bucket,
    const std::string& key,
    uint64_t expires_in_seconds) {

    Logger::instance().info("Generating OSS presigned URL for: " + bucket + "/" + key);
    return generate_presigned_url_internal_oss(bucket, key, expires_in_seconds, "");
}

std::string OSSClient::generate_presigned_url_internal_oss(
    const std::string& bucket,
    const std::string& key,
    uint64_t expires_in_seconds,
    const std::string& /*query_params*/) {

    const auto& config = get_config();
    auto* impl = get_impl();
    std::string access_key = impl ? impl->access_key : "";
    std::string secret_key = impl ? impl->secret_key : "";

    if (access_key.empty() || secret_key.empty()) {
        Logger::instance().warning("No credentials for OSS presigned URL");
        return get_object_url(bucket, key);
    }

    // Get endpoint and extract host
    std::string endpoint = config.endpoint;
    if (endpoint.substr(0, 8) == "https://") {
        endpoint = endpoint.substr(8);
    } else if (endpoint.substr(0, 7) == "http://") {
        endpoint = endpoint.substr(7);
    }

    // Remove port if present
    size_t colon_pos = endpoint.rfind(':');
    if (colon_pos != std::string::npos) {
        endpoint = endpoint.substr(0, colon_pos);
    }

    std::string expires = std::to_string(expires_in_seconds);
    std::string date = get_current_time_oss();

    // Build string to sign (OSS style)
    std::ostringstream string_to_sign;
    string_to_sign << "GET\n\n\n";
    string_to_sign << expires << "\n";
    string_to_sign << "/" << bucket << "/" << key;

    std::string signature = hmac_sha1(secret_key, string_to_sign.str());

    // Build final URL
    std::string protocol = config.use_https ? "https://" : "http://";
    std::string url = protocol + bucket + "." + endpoint + "/" + key;
    url += "?OSSAccessKeyId=" + access_key;
    url += "&Expires=" + expires;
    url += "&Signature=" + url_encode_oss(signature);

    return url;
}

std::vector<std::pair<std::string, std::string>> OSSClient::sign_request_oss(
    const std::string& method,
    const std::string& /*bucket*/,
    const std::string& /*key*/,
    const std::string& /*query_string*/,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& /*payload_hash*/) {

    auto* impl = get_impl();
    std::string access_key = impl ? impl->access_key : "";
    std::string secret_key = impl ? impl->secret_key : "";

    if (access_key.empty() || secret_key.empty()) {
        Logger::instance().warning("No credentials for OSS signing");
        return {};
    }

    std::string date = get_current_time_oss();

    // Build canonical string for OSS
    std::ostringstream canonical_string;
    canonical_string << method << "\n";
    canonical_string << "\n";  // Content-MD5 (empty)
    canonical_string << "\n";  // Content-Type (empty)
    canonical_string << date << "\n";

    // Add OSS-specific headers
    std::map<std::string, std::string> oss_headers;
    for (const auto& h : headers) {
        std::string lower_key;
        for (char c : h.first) {
            lower_key += static_cast<char>(std::tolower(c));
        }
        if (lower_key.substr(0, 4) == "x-oss") {
            oss_headers[lower_key] = h.second;
        }
    }

    // Add OSS headers in sorted order
    for (const auto& h : oss_headers) {
        canonical_string << h.first << ":" << h.second << "\n";
    }

    std::string signature = hmac_sha1(secret_key, canonical_string.str());

    return {
        {"Date", date},
        {"Authorization", "OSS " + access_key + ":" + signature}
    };
}

OSSClient::AuthenticatedRequest OSSClient::authenticate_request_oss(
    const std::string& method,
    const std::string& bucket,
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& original_headers,
    const std::string& original_query_string) {

    AuthenticatedRequest auth_req;
    auth_req.url = get_object_url(bucket, key);
    auth_req.needs_passthrough = false;

    auto* impl = get_impl();
    std::string access_key = impl ? impl->access_key : "";
    std::string secret_key = impl ? impl->secret_key : "";

    // Check if there's an OSS signature in query string
    if (!original_query_string.empty()) {
        // Check if already has OSS signature params
        bool has_oss_sig = (original_query_string.find("OSSAccessKeyId") != std::string::npos ||
                          original_query_string.find("Signature") != std::string::npos);

        if (has_oss_sig && !access_key.empty() && !secret_key.empty()) {
            // Re-generate presigned URL with our credentials
            auth_req.url = generate_presigned_url_internal_oss(bucket, key, 3600, original_query_string);
        } else if (!has_oss_sig) {
            // Pass through the original URL with its params
            auth_req.url = auth_req.url + "?" + original_query_string;
        } else {
            // Pass through the original URL with signature
            auth_req.url = auth_req.url + "?" + original_query_string;
        }
    }
    // Check for OSS header-based authentication
    else {
        bool has_oss_auth = false;
        for (const auto& h : original_headers) {
            if (h.first == "Authorization") {
                has_oss_auth = true;
                break;
            }
        }

        if (has_oss_auth) {
            if (!access_key.empty() && !secret_key.empty()) {
                // Sign with our credentials
                auto signed_headers = sign_request_oss(method, bucket, key, "", original_headers, "");
                auth_req.headers = original_headers;
                for (const auto& h : signed_headers) {
                    auth_req.headers.push_back(h);
                }
            } else {
                // Pass through original signed headers
                auth_req.headers = original_headers;
            }
        } else {
            // No authentication - public bucket
            auth_req.needs_passthrough = access_key.empty();
        }
    }

    return auth_req;
}

bool OSSClient::set_bucket_lifecycle(
    const std::string& /*bucket*/,
    const std::string& /*rule_id*/,
    int /*days_after_expiration*/) {

    Logger::instance().info("Setting lifecycle rule for bucket");
    // Placeholder: would implement OSS API call
    return true;
}

bool OSSClient::enable_bucket_cors(
    const std::string& /*bucket*/,
    const std::vector<std::string>& /*allowed_origins*/,
    const std::vector<std::string>& /*allowed_methods*/) {

    Logger::instance().info("Enabling CORS for bucket");
    // Placeholder: would implement OSS API call
    return true;
}

} // namespace eliop2p
