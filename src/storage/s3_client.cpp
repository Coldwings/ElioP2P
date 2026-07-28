#include "eliop2p/storage/s3_client.h"
#include "eliop2p/storage/oss_client.h"
#include "eliop2p/base/logger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <elio/http/http_client.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <map>
#include <cctype>

namespace eliop2p {

// Forward declarations for helper functions
class S3Client;
static std::optional<ListObjectsResult> parse_list_objects_response(const std::string& body);

// SHA256 hash helper
static std::string sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// HMAC-SHA256 returning the raw 32-byte digest. The SigV4 key-derivation
// chain must feed BINARY digests forward - hex-encoding intermediate keys
// silently derives completely different signing keys.
static std::string hmac_sha256_raw(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), key.size(),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash, &len);
    return std::string(reinterpret_cast<char*>(hash), len);
}

// HMAC-SHA256 returning lowercase hex (for the final signature value only)
static std::string hmac_sha256(const std::string& key, const std::string& data) {
    std::string raw = hmac_sha256_raw(key, data);
    std::ostringstream oss;
    for (unsigned char c : raw) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return oss.str();
}

// Get current UTC time in ISO 8601 basic format required by AWS SigV4
// (YYYYMMDDTHHMMSSZ, no fractional seconds).
static std::string get_current_time_iso() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y%m%dT%H%M%SZ");
    return oss.str();
}

// Get current date in YYYYMMDD format
static std::string get_current_date() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y%m%d");
    return oss.str();
}

// URL encode for S3 (path-style, not query-string encoding)
static std::string url_encode_path(const std::string& value) {
    std::ostringstream oss;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            oss << c;
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return oss.str();
}

// URL encode for SigV4 query parameter names/values (RFC 3986 unreserved
// characters only; unlike path encoding, '/' is percent-encoded).
static std::string url_encode_query(const std::string& value) {
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

// Trim leading/trailing whitespace and collapse internal runs of whitespace
// into a single space, as required for SigV4 canonical header values.
static std::string trim_and_compress(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool pending_space = false;
    bool seen_any = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            if (seen_any) pending_space = true;
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += static_cast<char>(c);
        seen_any = true;
    }
    return out;
}

// Build a SigV4 canonical query string from a raw "a=1&b=2" style string:
// parameters are URI-encoded and sorted by encoded name.
static std::string canonicalize_query_string(const std::string& query) {
    std::map<std::string, std::string> params;  // encoded name -> encoded value
    std::istringstream iss(query);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        std::string name = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        std::string value = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
        params[url_encode_query(name)] = url_encode_query(value);
    }
    std::ostringstream oss;
    bool first = true;
    for (const auto& [name, value] : params) {
        if (!first) oss << '&';
        first = false;
        oss << name << '=' << value;
    }
    return oss.str();
}

// Decode the standard XML predefined entities in a parsed value.
static std::string xml_unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '&') {
            size_t semi = value.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 6) {
                std::string entity = value.substr(i, semi - i + 1);
                if (entity == "&amp;")       { out += '&';  i = semi + 1; continue; }
                if (entity == "&lt;")        { out += '<';  i = semi + 1; continue; }
                if (entity == "&gt;")        { out += '>';  i = semi + 1; continue; }
                if (entity == "&quot;")      { out += '"';  i = semi + 1; continue; }
                if (entity == "&#39;" ||
                    entity == "&apos;")      { out += '\''; i = semi + 1; continue; }
            }
        }
        out += value[i];
        ++i;
    }
    return out;
}

// Parse endpoint to get host and port
static std::pair<std::string, uint16_t> parse_endpoint(const std::string& endpoint, bool use_https) {
    std::string host = endpoint;
    uint16_t port = use_https ? 443 : 80;

    // Remove protocol if present
    if (host.substr(0, 8) == "https://") {
        host = host.substr(8);
        port = 443;
    } else if (host.substr(0, 7) == "http://") {
        host = host.substr(7);
        port = 80;
    }

    // Check for port in endpoint
    size_t colon_pos = host.rfind(':');
    if (colon_pos != std::string::npos) {
        try {
            port = static_cast<uint16_t>(std::stoi(host.substr(colon_pos + 1)));
            host = host.substr(0, colon_pos);
        } catch (...) {
            // Ignore parsing errors
        }
    }

    return {host, port};
}

S3Client::S3Client(const StorageConfig& config)
    : impl_(std::make_unique<Impl>()), config_(config) {
    if (config.access_key && config.secret_key) {
        impl_->access_key = *config.access_key;
        impl_->secret_key = *config.secret_key;
        access_key_ = *config.access_key;
        secret_key_ = *config.secret_key;
    }
    // An explicit scheme in the endpoint overrides use_https: without this,
    // "http://minio:9000" with the default use_https=true would attempt a
    // TLS handshake against a plaintext listener.
    if (config_.endpoint.rfind("http://", 0) == 0) {
        config_.use_https = false;
    } else if (config_.endpoint.rfind("https://", 0) == 0) {
        config_.use_https = true;
    }
    Logger::instance().info("S3Client initialized with endpoint: " + config.endpoint);
}

S3Client::~S3Client() = default;

void S3Client::set_credentials(const std::string& access_key, const std::string& secret_key) {
    impl_->access_key = access_key;
    impl_->secret_key = secret_key;
    access_key_ = access_key;
    secret_key_ = secret_key;
    auth_type_ = AuthType::HeaderSigned;
}

std::string S3Client::build_object_url(
    const std::string& bucket,
    const std::string& key,
    bool use_path_style) const {

    std::string endpoint = config_.endpoint;
    std::string protocol = config_.use_https ? "https://" : "http://";

    // Remove protocol from endpoint if present
    if (endpoint.substr(0, 8) == "https://") {
        endpoint = endpoint.substr(8);
    } else if (endpoint.substr(0, 7) == "http://") {
        endpoint = endpoint.substr(7);
    }

    if (use_path_style) {
        // Path-style: http://endpoint/bucket/key
        return protocol + endpoint + "/" + bucket + "/" + key;
    } else {
        // Virtual-hosted-style: http://bucket.endpoint/key
        return protocol + bucket + "." + endpoint + "/" + key;
    }
}

std::vector<std::pair<std::string, std::string>> S3Client::sign_request(
    const std::string& method,
    const std::string& bucket,
    const std::string& key,
    const std::string& query_string,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& payload_hash) {

    if (impl_->access_key.empty() || impl_->secret_key.empty()) {
        Logger::instance().warning("No credentials provided for signing");
        return {};
    }

    // Parse endpoint to get host
    auto [host, port] = parse_endpoint(config_.endpoint, config_.use_https);
    std::string region = config_.region.empty() ? "us-east-1" : config_.region;
    const std::string service = "s3";
    std::string amz_date = get_current_time_iso();
    std::string date_stamp = get_current_date();

    // The Host header carries the port only when it is not the default for
    // the scheme, and it appears exactly once in the canonical headers while
    // SignedHeaders lists it as plain "host".
    const bool default_port =
        (config_.use_https && port == 443) || (!config_.use_https && port == 80);
    std::string host_header = host;
    if (!default_port) {
        host_header += ":" + std::to_string(port);
    }

    // Collect canonical headers keyed by lowercase header name. std::map
    // keeps them sorted by name as SigV4 requires; values are trimmed and
    // internal whitespace runs are collapsed to a single space.
    std::map<std::string, std::string> canonical_map;
    canonical_map["host"] = host_header;
    canonical_map["x-amz-content-sha256"] = payload_hash;
    canonical_map["x-amz-date"] = amz_date;
    for (const auto& h : headers) {
        std::string lower_key;
        for (char c : h.first) {
            lower_key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        canonical_map[lower_key] = trim_and_compress(h.second);
    }

    std::ostringstream canonical_headers;
    std::ostringstream signed_headers_ss;
    bool first = true;
    for (const auto& [name, value] : canonical_map) {
        canonical_headers << name << ":" << value << "\n";
        if (!first) signed_headers_ss << ";";
        first = false;
        signed_headers_ss << name;
    }
    // SignedHeaders is generated from the exact set of canonical headers,
    // never hardcoded, so the two can never drift apart.
    const std::string signed_headers = signed_headers_ss.str();

    // Canonical URI: service root for bucket-less requests, otherwise
    // path-style /bucket/key with the key URI-encoded per SigV4 rules.
    std::string canonical_uri;
    if (bucket.empty()) {
        canonical_uri = "/";
    } else {
        canonical_uri = "/" + bucket + "/";
        if (!key.empty()) {
            canonical_uri += url_encode_path(key);
        }
    }

    // Build canonical request
    std::ostringstream canonical_request;
    canonical_request << method << "\n";
    canonical_request << canonical_uri << "\n";
    canonical_request << query_string << "\n";
    canonical_request << canonical_headers.str() << "\n";
    canonical_request << signed_headers << "\n";
    canonical_request << payload_hash;

    std::string canonical_request_hash = sha256_hex(canonical_request.str());
    Logger::instance().debug("SigV4 canonical request:\n" + canonical_request.str());

    // Build string to sign
    std::ostringstream string_to_sign;
    string_to_sign << "AWS4-HMAC-SHA256\n";
    string_to_sign << amz_date << "\n";
    string_to_sign << date_stamp << "/" << region << "/" << service << "/aws4_request\n";
    string_to_sign << canonical_request_hash;

    // Calculate signature. The derivation chain feeds raw binary digests;
    // only the final signature is hex-encoded.
    std::string k_secret = "AWS4" + impl_->secret_key;
    std::string k_date = hmac_sha256_raw(k_secret, date_stamp);
    std::string k_region = hmac_sha256_raw(k_date, region);
    std::string k_service = hmac_sha256_raw(k_region, service);
    std::string k_signing = hmac_sha256_raw(k_service, "aws4_request");
    std::string signature = hmac_sha256(k_signing, string_to_sign.str());

    // Build authorization header
    std::ostringstream auth_header;
    auth_header << "AWS4-HMAC-SHA256 ";
    auth_header << "Credential=" << impl_->access_key << "/" << date_stamp << "/" << region << "/" << service << "/aws4_request, ";
    auth_header << "SignedHeaders=" << signed_headers << ", ";
    auth_header << "Signature=" << signature;

    return {
        {"x-amz-date", amz_date},
        {"x-amz-content-sha256", payload_hash},
        {"Authorization", auth_header.str()}
    };
}

std::string S3Client::generate_presigned_url_internal(
    const std::string& bucket,
    const std::string& key,
    uint64_t expires_in_seconds,
    const std::string& query_params) {

    if (impl_->access_key.empty() || impl_->secret_key.empty()) {
        Logger::instance().warning("No credentials for presigned URL");
        return build_object_url(bucket, key);
    }

    auto [host, port] = parse_endpoint(config_.endpoint, config_.use_https);
    std::string region = config_.region.empty() ? "us-east-1" : config_.region;

    std::string amz_date = get_current_time_iso();
    std::string date_stamp = get_current_date();

    // SigV4 presigned URL query parameters (raw values; they are URI-encoded
    // when the canonical query string is emitted).
    std::map<std::string, std::string> query_params_map;
    query_params_map["X-Amz-Algorithm"] = "AWS4-HMAC-SHA256";
    query_params_map["X-Amz-Credential"] = impl_->access_key + "/" + date_stamp + "/" + region + "/s3/aws4_request";
    query_params_map["X-Amz-Date"] = amz_date;
    query_params_map["X-Amz-Expires"] = std::to_string(expires_in_seconds);
    query_params_map["X-Amz-SignedHeaders"] = "host";

    // Add any additional query params
    if (!query_params.empty()) {
        std::istringstream iss(query_params);
        std::string param;
        while (std::getline(iss, param, '&')) {
            size_t eq_pos = param.find('=');
            if (eq_pos != std::string::npos) {
                query_params_map[param.substr(0, eq_pos)] = param.substr(eq_pos + 1);
            }
        }
    }

    // Build canonical query string: parameters sorted by name, names and
    // values URI-encoded per RFC 3986 (unreserved characters only).
    std::ostringstream canonical_querystring;
    bool first = true;
    for (const auto& p : query_params_map) {
        if (!first) canonical_querystring << "&";
        first = false;
        canonical_querystring << url_encode_query(p.first) << "=" << url_encode_query(p.second);
    }

    // Canonical headers for a presigned URL contain only the host header
    // (with the port when it is not the scheme default).
    const bool default_port =
        (config_.use_https && port == 443) || (!config_.use_https && port == 80);
    std::string host_header = host;
    if (!default_port) {
        host_header += ":" + std::to_string(port);
    }

    // Build canonical request
    std::ostringstream canonical_request;
    canonical_request << "GET\n";
    canonical_request << "/" << bucket << "/" << url_encode_path(key) << "\n";
    canonical_request << canonical_querystring.str() << "\n";
    canonical_request << "host:" << host_header << "\n";
    canonical_request << "\n";
    canonical_request << "host\n";
    canonical_request << "UNSIGNED-PAYLOAD";

    std::string canonical_request_hash = sha256_hex(canonical_request.str());

    // Build string to sign
    std::ostringstream string_to_sign;
    string_to_sign << "AWS4-HMAC-SHA256\n";
    string_to_sign << amz_date << "\n";
    string_to_sign << date_stamp << "/" << region << "/s3/aws4_request\n";
    string_to_sign << canonical_request_hash;

    // Calculate signature
    std::string k_secret = "AWS4" + impl_->secret_key;
    std::string k_date = hmac_sha256_raw(k_secret, date_stamp);
    std::string k_region = hmac_sha256_raw(k_date, region);
    std::string k_service = hmac_sha256_raw(k_region, "s3");
    std::string k_signing = hmac_sha256_raw(k_service, "aws4_request");
    std::string signature = hmac_sha256(k_signing, string_to_sign.str());

    // Build final URL
    std::string protocol = config_.use_https ? "https://" : "http://";
    std::string url = protocol + host_header;
    url += "/" + bucket + "/" + url_encode_path(key) + "?" + canonical_querystring.str() + "&X-Amz-Signature=" + signature;

    return url;
}

S3Client::AuthenticatedRequest S3Client::authenticate_request(
    const std::string& method,
    const std::string& bucket,
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& original_headers,
    const std::string& original_query_string) {

    AuthenticatedRequest auth_req;
    auth_req.url = build_object_url(bucket, key);
    auth_req.needs_passthrough = false;

    // Check if there's a presigned URL signature in query string
    if (!original_query_string.empty()) {
        // If we have credentials, we can re-sign the request
        // Otherwise, we pass through the original URL with signature
        if (!impl_->access_key.empty() && !impl_->secret_key.empty()) {
            // Re-generate presigned URL with our credentials
            auth_req.url = generate_presigned_url_internal(bucket, key, 3600, original_query_string);
        } else {
            // Pass through the original URL with its signature
            auth_req.url = auth_req.url + "?" + original_query_string;
        }
    }
    // Check for header-based authentication (x-amz-date)
    else {
        bool has_amz_date = false;
        for (const auto& h : original_headers) {
            if (h.first == "x-amz-date" || h.first == "Authorization") {
                has_amz_date = true;
                break;
            }
        }

        if (has_amz_date) {
            if (!impl_->access_key.empty() && !impl_->secret_key.empty()) {
                // Sign with our credentials
                std::string payload_hash = sha256_hex("");
                auto signed_headers = sign_request(method, bucket, key, "", original_headers, payload_hash);
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
            auth_req.needs_passthrough = (impl_->access_key.empty());
        }
    }

    return auth_req;
}

bool S3Client::test_connection() {
    Logger::instance().info("Testing connection to S3: " + config_.endpoint);
    return !config_.endpoint.empty();
}

elio::coro::task<std::optional<std::vector<std::string>>> S3Client::list_buckets() {
    Logger::instance().info("Listing buckets");

    // For S3, list buckets is a GET request to the root with no bucket name
    // Build URL without bucket
    std::string endpoint = config_.endpoint;
    std::string protocol = config_.use_https ? "https://" : "http://";

    // Remove protocol from endpoint if present
    if (endpoint.substr(0, 8) == "https://") {
        endpoint = endpoint.substr(8);
    } else if (endpoint.substr(0, 7) == "http://") {
        endpoint = endpoint.substr(7);
    }

    std::string url = protocol + endpoint + "/";

    // Calculate payload hash
    std::string payload_hash = sha256_hex("");

    // Sign request
    std::vector<std::pair<std::string, std::string>> signed_headers;
    if (!impl_->access_key.empty() && !impl_->secret_key.empty()) {
        signed_headers = sign_request("GET", "", "", "", {}, payload_hash);
    } else {
        Logger::instance().warning(
            "No storage credentials configured; sending unsigned list_buckets "
            "request (only works for public buckets / anonymous access)");
    }

    elio::http::client http_client;
    elio::http::request req(elio::http::method::GET, "/");

    auto [host, port] = parse_endpoint(config_.endpoint, config_.use_https);
    {
        const bool default_port =
            (config_.use_https && port == 443) || (!config_.use_https && port == 80);
        req.set_host(default_port ? host : host + ":" + std::to_string(port));
    }

    for (const auto& h : signed_headers) {
        req.set_header(h.first, h.second);
    }

    auto parsed_url = elio::http::url::parse(url);
    if (!parsed_url) {
        Logger::instance().error("Failed to parse URL: " + url);
        co_return std::nullopt;
    }

    auto response = co_await http_client.send(req, *parsed_url);

    if (!response) {
        Logger::instance().error("Failed to list buckets");
        co_return std::nullopt;
    }

    // Check status code
    auto status = response->status_code();
    if (status != 200) {
        Logger::instance().error("List buckets failed with status: " + std::to_string(status));
        co_return std::nullopt;
    }

    // Parse XML response to extract bucket names
    std::string body_str = std::string(response->body());
    std::vector<std::string> buckets;

    // Simple XML parsing for bucket names
    size_t pos = 0;
    while (true) {
        size_t name_start = body_str.find("<Name>", pos);
        if (name_start == std::string::npos) break;
        size_t name_end = body_str.find("</Name>", name_start);
        if (name_end == std::string::npos) break;

        std::string name = body_str.substr(name_start + 6, name_end - name_start - 6);
        buckets.push_back(name);
        pos = name_end;
    }

    co_return buckets;
}

elio::coro::task<std::optional<ListObjectsResult>> S3Client::list_objects(
    const std::string& bucket,
    const std::string& prefix,
    const std::string& continuation_token,
    uint32_t max_keys) {

    Logger::instance().info("Listing objects in bucket: " + bucket);

    // Build query string with raw values; execute_request canonicalizes
    // (sorts and URI-encodes) it before signing and sending so the wire
    // format always matches the signature.
    std::ostringstream query;
    query << "list-type=2";
    if (!prefix.empty()) {
        query << "&prefix=" << prefix;
    }
    if (max_keys > 0) {
        query << "&max-keys=" << max_keys;
    }
    if (!continuation_token.empty()) {
        query << "&continuation-token=" << continuation_token;
    }

    auto response = co_await execute_request(
        "GET", bucket, "", query.str(), {}, "", "");

    if (!response) {
        Logger::instance().error("Failed to list objects in bucket: " + bucket);
        co_return std::nullopt;
    }

    // Check status code
    auto status = response->status_code();
    if (status != 200) {
        Logger::instance().error("List objects failed with status: " + std::to_string(status));
        co_return std::nullopt;
    }

    // Parse XML response
    std::string body_str = std::string(response->body());
    auto result = parse_list_objects_response(body_str);

    if (!result) {
        Logger::instance().error("Failed to parse list objects response");
        co_return std::nullopt;
    }

    co_return result;
}

elio::coro::task<std::optional<ObjectMetadata>> S3Client::head_object(
    const std::string& bucket,
    const std::string& key) {

    Logger::instance().debug("Head object: " + bucket + "/" + key);

    auto response = co_await execute_request(
        "HEAD", bucket, key, "", {}, "", "");

    if (!response) {
        Logger::instance().error("Failed to head object: " + bucket + "/" + key);
        co_return std::nullopt;
    }

    // Check status code
    auto status = response->status_code();
    if (status != 200 && status != 204) {
        if (status == 404) {
            Logger::instance().debug("Object not found: " + bucket + "/" + key);
        } else {
            Logger::instance().error("Head object failed with status: " + std::to_string(status));
            // MinIO/S3 include the expected StringToSign in the error body -
            // log it to make signature mismatches debuggable
            auto body = response->body();
            if (!body.empty()) {
                Logger::instance().error("Head object error body: " + std::string(body.substr(0, 2048)));
            }
        }
        co_return std::nullopt;
    }

    // Parse metadata from headers
    ObjectMetadata metadata;
    metadata.key = key;

    // Get Content-Length
    auto content_length = response->header("Content-Length");
    if (!content_length.empty()) {
        try {
            metadata.size = std::stoull(std::string(content_length));
        } catch (...) {}
    }

    // Get ETag
    auto etag = response->header("ETag");
    if (!etag.empty()) {
        metadata.etag = std::string(etag);
    }

    // Get Content-Type
    auto content_type = response->header("Content-Type");
    if (!content_type.empty()) {
        metadata.content_type = std::string(content_type);
    }

    // Get Last-Modified
    auto last_modified = response->header("Last-Modified");
    if (!last_modified.empty()) {
        metadata.last_modified = std::string(last_modified);
    }

    // Get x-amz-version-id
    auto version_id = response->header("x-amz-version-id");
    if (!version_id.empty()) {
        metadata.version_id = std::string(version_id);
    }

    co_return metadata;
}

elio::coro::task<std::optional<std::vector<uint8_t>>> S3Client::get_object(
    const std::string& bucket,
    const std::string& key,
    uint64_t offset,
    uint64_t length) {

    Logger::instance().info("Get object: " + bucket + "/" + key +
                           " offset=" + std::to_string(offset) +
                           " length=" + std::to_string(length));

    // Build Range header if needed
    std::vector<std::pair<std::string, std::string>> extra_headers;
    if (offset > 0 || length > 0) {
        std::ostringstream range_header;
        range_header << "bytes=" << offset;
        if (length > 0) {
            range_header << "-" << (offset + length - 1);
        }
        extra_headers.push_back({"Range", range_header.str()});
    }

    auto response = co_await execute_request(
        "GET", bucket, key, "", extra_headers, "", "");

    if (!response) {
        Logger::instance().error("Failed to get object: " + bucket + "/" + key);
        co_return std::nullopt;
    }

    // Check status code
    auto status = response->status_code();
    if (status != 200 && status != 206) {
        Logger::instance().error("Get object failed with status: " + std::to_string(status));
        auto err_body = response->body();
        if (!err_body.empty()) {
            Logger::instance().error("Get object error body: " + std::string(err_body.substr(0, 3000)));
        }
        co_return std::nullopt;
    }

    // Get body as vector
    std::string body_str = std::string(response->body());
    std::vector<uint8_t> result(body_str.begin(), body_str.end());

    co_return result;
}

elio::coro::task<bool> S3Client::put_object(
    const std::string& bucket,
    const std::string& key,
    const std::vector<uint8_t>& data,
    const std::string& content_type) {

    Logger::instance().info("Put object: " + bucket + "/" + key +
                           " size=" + std::to_string(data.size()));

    // Convert data to string for HTTP request
    std::string body(data.begin(), data.end());

    auto response = co_await execute_request(
        "PUT", bucket, key, "", {}, body, content_type);

    if (!response) {
        Logger::instance().error("Failed to put object: " + bucket + "/" + key);
        co_return false;
    }

    // Check status code - 200 OK or 201 Created
    auto status = response->status_code();
    if (status != 200 && status != 201) {
        Logger::instance().error("Put object failed with status: " + std::to_string(status));
        // Log error response body for debugging
        if (!response->body().empty()) {
            Logger::instance().error("Error response: " + std::string(response->body()));
        }
        co_return false;
    }

    co_return true;
}

elio::coro::task<bool> S3Client::delete_object(
    const std::string& bucket,
    const std::string& key) {

    Logger::instance().info("Delete object: " + bucket + "/" + key);

    auto response = co_await execute_request(
        "DELETE", bucket, key, "", {}, "", "");

    if (!response) {
        Logger::instance().error("Failed to delete object: " + bucket + "/" + key);
        co_return false;
    }

    // Check status code - 204 No Content or 200 OK
    auto status = response->status_code();
    if (status != 200 && status != 204 && status != 202) {
        Logger::instance().error("Delete object failed with status: " + std::to_string(status));
        co_return false;
    }

    co_return true;
}

std::string S3Client::generate_presigned_url(
    const std::string& bucket,
    const std::string& key,
    uint64_t expires_in_seconds) {

    Logger::instance().info("Generating presigned URL for: " + bucket + "/" + key);

    return generate_presigned_url_internal(bucket, key, expires_in_seconds, "");
}

elio::coro::task<bool> S3Client::bucket_exists(const std::string& bucket) {
    Logger::instance().info("Checking if bucket exists: " + bucket);

    // Try to list objects in the bucket with max-keys=1
    // This is a lightweight operation that will fail if bucket doesn't exist
    auto response = co_await execute_request(
        "GET", bucket, "", "max-keys=1", {}, "", "");

    if (!response) {
        co_return false;
    }

    // Only 200 OK proves the bucket exists and is accessible.
    auto status = response->status_code();
    if (status == 200) {
        co_return true;
    }
    if (status == 404) {
        co_return false;
    }

    // Any other status (403, 5xx, ...) means we could not confirm the bucket;
    // treat as "not existing" instead of silently claiming success.
    Logger::instance().error("bucket_exists check for '" + bucket +
                             "' failed with status: " + std::to_string(status));
    co_return false;
}

// Storage client factory implementation
StorageBackend StorageClientFactory::detect_backend(const std::string& endpoint) {
    std::string lower_endpoint;
    for (char c : endpoint) {
        lower_endpoint += static_cast<char>(std::tolower(c));
    }

    // Check for Alibaba Cloud OSS endpoints
    if (lower_endpoint.find("aliyuncs.com") != std::string::npos ||
        lower_endpoint.find(".aliyuncs.") != std::string::npos ||
        lower_endpoint.find("oss-cn-") != std::string::npos ||
        lower_endpoint.find("oss.aliyun") != std::string::npos) {
        return StorageBackend::OSS;
    }

    // Default to S3-compatible
    return StorageBackend::S3;
}

StorageBackend StorageClientFactory::detect_backend(const std::string& endpoint,
                                                   const std::optional<std::string>& access_key) {
    // First detect by endpoint
    StorageBackend backend = detect_backend(endpoint);

    // If no access key, assume OSS if endpoint suggests it
    // Otherwise default to S3
    if (!access_key && backend != StorageBackend::OSS) {
        // Check if endpoint matches common OSS patterns
        if (endpoint.find("aliyun") != std::string::npos ||
            endpoint.find("alibaba") != std::string::npos) {
            return StorageBackend::OSS;
        }
    }

    return backend;
}

std::unique_ptr<StorageClient> StorageClientFactory::create(const StorageConfig& config) {
    StorageBackend backend = detect_backend(config.endpoint, config.access_key);

    if (backend == StorageBackend::OSS) {
        Logger::instance().info("Creating OSS client for endpoint: " + config.endpoint);
        return std::make_unique<OSSClient>(config);
    }

    Logger::instance().info("Creating S3 client for endpoint: " + config.endpoint);
    return std::make_unique<S3Client>(config);
}

void S3Client::set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler) {
    // No-op: elio::http::client always uses the ambient
    // elio::runtime::scheduler::current() of the calling coroutine and offers
    // no way to inject an external scheduler. Kept for interface
    // compatibility only.
    (void)scheduler;
}

// Helper to execute HTTP request with S3 signing
elio::coro::task<std::optional<elio::http::response>> S3Client::execute_request(
    const std::string& method,
    const std::string& bucket,
    const std::string& key,
    const std::string& query_string,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    const std::string& body,
    const std::string& content_type) {

    // Build URL
    std::string url = build_object_url(bucket, key, true);

    // Calculate payload hash (empty-body SHA256 for GET/HEAD and other
    // bodyless requests, per SigV4).
    std::string payload_hash;
    if (body.empty()) {
        payload_hash = sha256_hex("");
    } else {
        payload_hash = sha256_hex(body);
    }

    // Canonicalize the query string once and use the identical form both on
    // the wire and inside the signature.
    const std::string canonical_query = canonicalize_query_string(query_string);

    // Sign request. Without credentials we deliberately fall back to an
    // unsigned request so public buckets and anonymous MinIO access keep
    // working; make the downgrade visible in the log.
    std::vector<std::pair<std::string, std::string>> signed_headers;
    if (!impl_->access_key.empty() && !impl_->secret_key.empty()) {
        signed_headers = sign_request(method, bucket, key, canonical_query, extra_headers, payload_hash);
    } else {
        Logger::instance().warning(
            "No storage credentials configured; sending unsigned " + method +
            " request for " + bucket + "/" + key +
            " (only works for public buckets / anonymous access)");
    }

    elio::http::client http_client;

    // Build request headers
    elio::http::request req(elio::http::string_to_method(method).value_or(elio::http::method::GET), "/" + bucket + "/" + url_encode_path(key));

    // Add query string if present
    if (!canonical_query.empty()) {
        req.set_query(canonical_query);
    }

    // Set host header. The Host header on the wire MUST match the host used
    // in the signature's canonical headers, including a non-default port -
    // otherwise the server recomputes a different signature (403).
    auto [host, port] = parse_endpoint(config_.endpoint, config_.use_https);
    {
        const bool default_port =
            (config_.use_https && port == 443) || (!config_.use_https && port == 80);
        req.set_host(default_port ? host : host + ":" + std::to_string(port));
    }

    // Add signed headers
    for (const auto& h : signed_headers) {
        req.set_header(h.first, h.second);
    }

    // Add extra headers
    for (const auto& h : extra_headers) {
        req.set_header(h.first, h.second);
    }

    // Add content type if specified
    if (!content_type.empty()) {
        req.set_content_type(content_type);
    }

    // Set body if present
    if (!body.empty()) {
        req.set_body(body);
    }

    // Make request
    auto parsed_url = elio::http::url::parse(url);
    if (!parsed_url) {
        Logger::instance().error("Failed to parse URL: " + url);
        co_return std::nullopt;
    }

    auto response = co_await http_client.send(req, *parsed_url);

    co_return response;
}

// Extract the (entity-decoded) text of the first <tag>...</tag> inside the
// given segment. Returns empty string when the tag is absent.
static std::string xml_extract_tag(const std::string& segment, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";
    size_t start = segment.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    size_t end = segment.find(close, start);
    if (end == std::string::npos) return "";
    return xml_unescape(segment.substr(start, end - start));
}

// Parse ListObjects XML response
static std::optional<ListObjectsResult> parse_list_objects_response(const std::string& body) {
    ListObjectsResult result;

    // Simple XML parsing for common cases
    // Look for <IsTruncated>true</IsTruncated>
    if (body.find("<IsTruncated>true</IsTruncated>") != std::string::npos) {
        result.is_truncated = true;
    }

    // Look for <NextContinuationToken>
    size_t next_token_pos = body.find("<NextContinuationToken>");
    if (next_token_pos != std::string::npos) {
        size_t start = next_token_pos + 25;
        size_t end = body.find("</NextContinuationToken>", start);
        if (end != std::string::npos) {
            result.continuation_token = xml_unescape(body.substr(start, end - start));
        }
    }

    // Parse one object per <Contents>...</Contents> segment so that fields
    // of different objects can never be mixed up.
    size_t pos = 0;
    while (true) {
        size_t seg_start = body.find("<Contents>", pos);
        if (seg_start == std::string::npos) break;
        size_t seg_end = body.find("</Contents>", seg_start);
        if (seg_end == std::string::npos) break;

        std::string segment = body.substr(seg_start, seg_end - seg_start);

        ObjectMetadata obj;
        obj.key = xml_extract_tag(segment, "Key");
        obj.etag = xml_extract_tag(segment, "ETag");
        obj.size = 0;
        std::string size_str = xml_extract_tag(segment, "Size");
        if (!size_str.empty()) {
            try {
                obj.size = std::stoull(size_str);
            } catch (...) {}
        }
        std::string last_modified = xml_extract_tag(segment, "LastModified");
        if (!last_modified.empty()) {
            obj.last_modified = last_modified;
        }

        result.objects.push_back(std::move(obj));
        pos = seg_end;
    }

    // Common prefixes count only when they appear inside a
    // <CommonPrefixes>...</CommonPrefixes> segment (the request <Prefix>
    // echo at the top level must not be collected).
    pos = 0;
    while (true) {
        size_t seg_start = body.find("<CommonPrefixes>", pos);
        if (seg_start == std::string::npos) break;
        size_t seg_end = body.find("</CommonPrefixes>", seg_start);
        if (seg_end == std::string::npos) break;

        std::string segment = body.substr(seg_start, seg_end - seg_start);
        std::string prefix = xml_extract_tag(segment, "Prefix");
        if (!prefix.empty()) {
            result.common_prefixes.push_back(std::move(prefix));
        }
        pos = seg_end;
    }

    return result;
}

} // namespace eliop2p
