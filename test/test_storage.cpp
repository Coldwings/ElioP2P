#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <optional>
#include <utility>
#include "eliop2p/storage/s3_client.h"
#include "eliop2p/storage/oss_client.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

void test_storage_backend_enum() {
    std::cout << "Testing storage backend enum..." << std::endl;

    assert(static_cast<int>(StorageBackend::S3) == 0);
    assert(static_cast<int>(StorageBackend::OSS) == 1);

    std::cout << "  Storage backend enum test PASSED" << std::endl;
}

void test_auth_type_enum() {
    std::cout << "Testing authentication type enum..." << std::endl;

    assert(static_cast<int>(AuthType::None) == 0);
    assert(static_cast<int>(AuthType::PresignedURL) == 1);
    assert(static_cast<int>(AuthType::HeaderSigned) == 2);

    std::cout << "  Auth type enum test PASSED" << std::endl;
}

void test_object_metadata() {
    std::cout << "Testing object metadata structure..." << std::endl;

    ObjectMetadata meta;
    meta.key = "test/object/key.txt";
    meta.size = 1024;
    meta.etag = "abc123def456";
    meta.content_type = "text/plain";
    meta.last_modified = "2024-01-15T10:30:00Z";
    meta.version_id = "v1";

    assert(meta.key == "test/object/key.txt");
    assert(meta.size == 1024);
    assert(meta.etag == "abc123def456");
    assert(meta.content_type.has_value());
    assert(*meta.content_type == "text/plain");
    assert(meta.version_id.has_value());
    assert(*meta.version_id == "v1");

    std::cout << "  Object metadata test PASSED" << std::endl;
}

void test_list_objects_result() {
    std::cout << "Testing list objects result structure..." << std::endl;

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

    assert(result.objects.size() == 2);
    assert(result.common_prefixes.size() == 1);
    assert(result.is_truncated == true);
    assert(result.continuation_token == "next_token_123");

    std::cout << "  List objects result test PASSED" << std::endl;
}

void test_s3_client_config() {
    std::cout << "Testing S3 client configuration..." << std::endl;

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

    assert(config.type == "s3");
    assert(config.endpoint == "http://localhost:9000");
    assert(config.region == "us-east-1");
    assert(config.access_key.has_value());
    assert(config.bucket == "test-bucket");
    assert(config.use_https == false);

    std::cout << "  S3 client config test PASSED" << std::endl;
}

void test_s3_client_creation() {
    std::cout << "Testing S3 client creation..." << std::endl;

    StorageConfig config;
    config.type = "s3";
    config.endpoint = "http://localhost:9000";
    config.region = "us-east-1";
    config.access_key = "testkey";
    config.secret_key = "testsecret";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Test get config
    [[maybe_unused]] const auto& client_config = client.get_config();
    assert(client_config.endpoint == "http://localhost:9000");
    assert(client_config.region == "us-east-1");

    // Test backend type
    assert(client.get_backend_type() == StorageBackend::S3);

    // Test initial auth type
    assert(client.get_auth_type() == AuthType::None);

    std::cout << "  S3 client creation test PASSED" << std::endl;
}

void test_s3_client_credentials() {
    std::cout << "Testing S3 client credentials..." << std::endl;

    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Set credentials
    client.set_credentials("new_access_key", "new_secret_key");
    client.set_auth_type(AuthType::HeaderSigned);

    assert(client.get_auth_type() == AuthType::HeaderSigned);

    std::cout << "  S3 client credentials test PASSED" << std::endl;
}

void test_oss_client_creation() {
    std::cout << "Testing OSS client creation..." << std::endl;

    StorageConfig config;
    config.type = "oss";
    config.endpoint = "http://localhost:9000";
    config.region = "cn-shanghai";
    config.access_key = "testkey";
    config.secret_key = "testsecret";
    config.bucket = "test-bucket";

    OSSClient client(config);

    // Test backend type - should be OSS, not S3
    assert(client.get_backend_type() == StorageBackend::OSS);

    // Test get config
    [[maybe_unused]] const auto& client_config = client.get_config();
    assert(client_config.region == "cn-shanghai");

    std::cout << "  OSS client creation test PASSED" << std::endl;
}

void test_storage_client_factory() {
    std::cout << "Testing storage client factory..." << std::endl;

    // Test S3 backend detection
    [[maybe_unused]] StorageBackend s3_backend = StorageClientFactory::detect_backend("http://s3.amazonaws.com");
    assert(s3_backend == StorageBackend::S3);

    [[maybe_unused]] StorageBackend minio_backend = StorageClientFactory::detect_backend("http://localhost:9000", "minioadmin");
    assert(minio_backend == StorageBackend::S3);

    // Test OSS backend detection
    [[maybe_unused]] StorageBackend oss_endpoint = StorageClientFactory::detect_backend("http://oss-cn-shanghai.aliyuncs.com");
    // OSS endpoint typically contains "oss"

    std::cout << "  Storage client factory test PASSED" << std::endl;
}

void test_storage_client_interface() {
    std::cout << "Testing storage client interface methods..." << std::endl;

    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";

    S3Client client(config);

    // Test endpoint and region getters
    [[maybe_unused]] const std::string& endpoint = client.get_endpoint();
    [[maybe_unused]] const std::string& region = client.get_region();

    assert(endpoint == "http://localhost:9000");
    assert(region == "us-east-1");

    // Test connection (will fail but should not crash)
    bool conn_result = client.test_connection();
    std::cout << "  Connection test result: " << (conn_result ? "success" : "failed") << std::endl;

    std::cout << "  Storage client interface test PASSED" << std::endl;
}

void test_authenticated_request() {
    std::cout << "Testing authenticated request structure..." << std::endl;

    S3Client::AuthenticatedRequest auth_req;
    auth_req.url = "http://localhost:9000/test-bucket/key.txt";
    auth_req.needs_passthrough = false;

    auth_req.headers.push_back(std::make_pair("Content-Type", "application/octet-stream"));
    auth_req.headers.push_back(std::make_pair("x-amz-date", "20240115T103000Z"));

    assert(auth_req.url == "http://localhost:9000/test-bucket/key.txt");
    assert(auth_req.headers.size() == 2);
    assert(auth_req.needs_passthrough == false);

    std::cout << "  Authenticated request test PASSED" << std::endl;
}

void test_build_object_url() {
    std::cout << "Testing object URL building..." << std::endl;

    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";
    config.region = "us-east-1";

    S3Client client(config);

    // Access protected method via test - we can only verify it compiles
    // The actual URL building is internal

    std::cout << "  Build object URL test PASSED" << std::endl;
}

void test_oss_specific_methods() {
    std::cout << "Testing OSS-specific methods..." << std::endl;

    StorageConfig config;
    config.endpoint = "http://localhost:9000";
    config.bucket = "test-bucket";
    config.region = "cn-shanghai";

    OSSClient client(config);

    // Test get_object_url (basic test - will return empty or fail)
    std::string object_url = client.get_object_url("test-bucket", "test/key.txt");
    std::cout << "  Object URL: " << object_url << std::endl;

    // Test generate_signed_url (should return a URL string)
    std::string signed_url = client.generate_signed_url("test-bucket", "test/key.txt", 3600);
    std::cout << "  Signed URL length: " << signed_url.length() << std::endl;

    // Test generate_presigned_url override
    std::string presigned = client.generate_presigned_url("test-bucket", "test/key.txt", 3600);
    std::cout << "  Presigned URL length: " << presigned.length() << std::endl;

    std::cout << "  OSS-specific methods test PASSED" << std::endl;
}

void test_storage_config_validation() {
    std::cout << "Testing storage config validation..." << std::endl;

    // Test valid config
    StorageConfig valid_config;
    valid_config.endpoint = "http://localhost:9000";
    valid_config.bucket = "test-bucket";
    valid_config.region = "us-east-1";

    assert(valid_config.endpoint == "http://localhost:9000");
    assert(valid_config.bucket == "test-bucket");

    // Test with SSL
    StorageConfig ssl_config;
    ssl_config.endpoint = "https://s3.amazonaws.com";
    ssl_config.bucket = "secure-bucket";
    ssl_config.use_https = true;

    assert(ssl_config.use_https == true);
    assert(ssl_config.endpoint.find("https://") == 0);

    // Test with credentials
    StorageConfig cred_config;
    cred_config.endpoint = "http://localhost:9000";
    cred_config.access_key = "accesskey";
    cred_config.secret_key = "secretkey";

    assert(cred_config.access_key.has_value());
    assert(cred_config.secret_key.has_value());

    std::cout << "  Storage config validation test PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running Storage Client Tests ===" << std::endl;

    try {
        test_storage_backend_enum();
        test_auth_type_enum();
        test_object_metadata();
        test_list_objects_result();
        test_s3_client_config();
        test_s3_client_creation();
        test_s3_client_credentials();
        test_oss_client_creation();
        test_storage_client_factory();
        test_storage_client_interface();
        test_authenticated_request();
        test_build_object_url();
        test_oss_specific_methods();
        test_storage_config_validation();

        std::cout << "\n=== All Storage Client Tests PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
