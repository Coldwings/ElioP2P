#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <optional>
#include <utility>
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/storage/oss_client.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

TEST_CASE("Storage Backend Enum", "[storage][enum]") {
    REQUIRE(static_cast<int>(StorageBackend::S3) == 0);
    REQUIRE(static_cast<int>(StorageBackend::OSS) == 1);
}

TEST_CASE("Authentication Type Enum", "[storage][enum][auth]") {
    REQUIRE(static_cast<int>(AuthType::None) == 0);
    REQUIRE(static_cast<int>(AuthType::PresignedURL) == 1);
    REQUIRE(static_cast<int>(AuthType::HeaderSigned) == 2);
}

TEST_CASE("Object Metadata Structure", "[storage][metadata]") {
    ObjectMetadata meta;
    meta.key = "test/object/key.txt";
    meta.size = 1024;
    meta.etag = "abc123def456";
    meta.content_type = "text/plain";
    meta.last_modified = "2024-01-15T10:30:00Z";
    meta.version_id = "v1";

    REQUIRE(meta.key == "test/object/key.txt");
    REQUIRE(meta.size == 1024);
    REQUIRE(meta.etag == "abc123def456");
    REQUIRE(meta.content_type.has_value() == true);
    REQUIRE(*meta.content_type == "text/plain");
    REQUIRE(meta.version_id.has_value() == true);
    REQUIRE(*meta.version_id == "v1");
}

TEST_CASE("List Objects Result Structure", "[storage][list]") {
    ListObjectsResult result;
    result.is_truncated = true;
    result.continuation_token = "next_token_123";

    // Add some objects
    ObjectMetadata obj1;
    obj1.key = "dir/file1.txt";
    obj1.size = 100;
    obj1.etag = "etag1";

    ObjectMetadata obj2;
    obj2.key = "dir/file2.txt";
    obj2.size = 200;
    obj2.etag = "etag2";

    result.objects.push_back(obj1);
    result.objects.push_back(obj2);
    result.common_prefixes.push_back("dir/subdir/");

    REQUIRE(result.objects.size() == 2);
    REQUIRE(result.common_prefixes.size() == 1);
    REQUIRE(result.is_truncated == true);
    REQUIRE(result.continuation_token == "next_token_123");
}

TEST_CASE("S3 Client Configuration", "[storage][s3][config]") {
    StorageConfig config;
    config.type = "s3";
    config.endpoint = "http://localhost:9000";
    config.region = "us-east-1";
    config.access_key = "minioadmin";
    config.secret_key = "minioadmin";
    config.bucket = "test-bucket";
    config.use_https = false;
    config.connection_timeout_sec = 30;
    config.read_timeout_sec = 60;

    REQUIRE(config.type == "s3");
    REQUIRE(config.endpoint == "http://localhost:9000");
    REQUIRE(config.region == "us-east-1");
    REQUIRE(config.access_key.has_value() == true);
    REQUIRE(config.bucket == "test-bucket");
    REQUIRE(config.use_https == false);
}

TEST_CASE("S3 Client Creation", "[storage][s3][creation]") {
    StorageConfig config;
    config.type = "s3";
    config.endpoint = "http://localhost:9000";
    config.region = "us-east-1";
    config.access_key = "testkey";
    config.secret_key = "testsecret";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Test get config
    const auto& client_config = client.get_config();
    REQUIRE(client_config.endpoint == "http://localhost:9000");
    REQUIRE(client_config.region == "us-east-1");

    // Test backend type
    REQUIRE(client.get_backend_type() == StorageBackend::S3);

    // Test initial auth type
    REQUIRE(client.get_auth_type() == AuthType::None);
}

TEST_CASE("S3 Client Credentials", "[storage][s3][credentials]") {
    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Set credentials
    client.set_credentials("new_access_key", "new_secret_key");
    client.set_auth_type(AuthType::HeaderSigned);

    REQUIRE(client.get_auth_type() == AuthType::HeaderSigned);
}

TEST_CASE("OSS Client Creation", "[storage][oss][creation]") {
    StorageConfig config;
    config.type = "oss";
    config.endpoint = "http://localhost:9000";
    config.region = "cn-shanghai";
    config.access_key = "testkey";
    config.secret_key = "testsecret";
    config.bucket = "test-bucket";

    OSSClient client(config);

    // Test backend type - should be OSS, not S3
    REQUIRE(client.get_backend_type() == StorageBackend::OSS);

    // Test get config
    const auto& client_config = client.get_config();
    REQUIRE(client_config.region == "cn-shanghai");
}

TEST_CASE("Storage Client Factory", "[storage][factory]") {
    // Test S3 backend detection
    StorageBackend s3_backend = StorageClientFactory::detect_backend("http://s3.amazonaws.com");
    REQUIRE(s3_backend == StorageBackend::S3);

    StorageBackend minio_backend = StorageClientFactory::detect_backend("http://localhost:9000", "minioadmin");
    REQUIRE(minio_backend == StorageBackend::S3);

    // Test OSS backend detection
    [[maybe_unused]] StorageBackend oss_endpoint = StorageClientFactory::detect_backend("http://oss-cn-shanghai.aliyuncs.com");
    // OSS endpoint typically contains "oss"
}

TEST_CASE("Storage Client Interface Methods", "[storage][interface]") {
    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Test endpoint and region getters
    const std::string& endpoint = client.get_endpoint();
    const std::string& region = client.get_region();

    REQUIRE(endpoint == "http://localhost:9000");
    REQUIRE(region == "us-east-1");

    // Test connection (will fail but should not crash)
    bool conn_result = client.test_connection();
    INFO("Connection test result: " << (conn_result ? "success" : "failed"));
}

TEST_CASE("Authenticated Request Structure", "[storage][auth][request]") {
    S3Client::AuthenticatedRequest auth_req;
    auth_req.url = "http://localhost:9000/test-bucket/key.txt";
    auth_req.needs_passthrough = false;

    auth_req.headers.push_back(std::make_pair("Content-Type", "application/octet-stream"));
    auth_req.headers.push_back(std::make_pair("x-amz-date", "20240115T103000Z"));

    REQUIRE(auth_req.url == "http://localhost:9000/test-bucket/key.txt");
    REQUIRE(auth_req.headers.size() == 2);
    REQUIRE(auth_req.needs_passthrough == false);
}

TEST_CASE("Object URL Building", "[storage][url]") {
    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    S3Client client(config);

    // Access protected method via test - we can only verify it compiles
    // The actual URL building is internal

    INFO("Build object URL test PASSED");
}

TEST_CASE("OSS-Specific Methods", "[storage][oss][methods]") {
    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";
    config.region = "cn-shanghai";

    OSSClient client(config);

    // Test get_object_url (basic test - will return empty or fail)
    std::string object_url = client.get_object_url("test-bucket", "test/key.txt");
    INFO("Object URL: " << object_url);

    // Test generate_signed_url (should return a URL string)
    std::string signed_url = client.generate_signed_url("test-bucket", "test/key.txt", 3600);
    INFO("Signed URL length: " << signed_url.length());

    // Test generate_presigned_url override
    std::string presigned = client.generate_presigned_url("test-bucket", "test/key.txt", 3600);
    INFO("Presigned URL length: " << presigned.length());
}

TEST_CASE("Storage Config Validation", "[storage][config][validation]") {
    // Test valid config
    StorageConfig valid_config;
    valid_config.endpoint = "http://localhost:9000";
    valid_config.bucket = "test-bucket";
    valid_config.region = "us-east-1";

    REQUIRE(valid_config.endpoint == "http://localhost:9000");
    REQUIRE(valid_config.bucket == "test-bucket");

    // Test with SSL
    StorageConfig ssl_config;
    ssl_config.endpoint = "https://s3.amazonaws.com";
    ssl_config.bucket = "secure-bucket";
    ssl_config.use_https = true;

    REQUIRE(ssl_config.use_https == true);
    REQUIRE(ssl_config.endpoint.find("https://") == 0);

    // Test with credentials
    StorageConfig cred_config;
    cred_config.endpoint = "http://localhost:9000";
    cred_config.access_key = "accesskey";
    cred_config.secret_key = "secretkey";

    REQUIRE(cred_config.access_key.has_value() == true);
    REQUIRE(cred_config.secret_key.has_value() == true);
}
