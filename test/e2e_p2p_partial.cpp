// E2E: on-demand (partial) reads of a large object over P2P.
//
// The scenario this proves: NO node on the network ever holds the complete
// 40MB object, yet every read succeeds - each chunk resolves independently
// (local cache -> P2P -> storage), and the X-Chunk-Sources response header
// shows exactly which tier served which chunk.
//
// Object: photos/2024/large_40mb.bin = chunk0(16MB) + chunk1(16MB) + chunk2(8MB)
//   Node B pre-warms ONLY chunk0 (a 1MB range read).
//   Node A starts empty; its storage backend is LIVE (it can backhaul).
//
// Steps:
//   1. B pre-warms chunk0 only; B provably does NOT have chunk1/chunk2
//   2. A reads a slice of chunk0           -> 0:P2P
//   3. A reads a slice of chunk1           -> 1:STORAGE (B doesn't have it)
//   4. A reads a range spanning chunk0+1   -> 0:CACHE,1:CACHE (both local now)
//      ...and A still does NOT have chunk2: on-demand granularity proof
//   5. A reads the FULL object             -> 0:CACHE,1:CACHE,2:STORAGE
//      (incomplete local state, mixed-tier single response)
//   6. B reads a slice of chunk1           -> 1:P2P (B pulls it from A -
//      the chunk A just backhauled and announced: reverse distribution)
//
// Usage: ./bin/e2e_p2p_partial  (requires MinIO :9090 with test-bucket)

#include "eliop2p/base/config.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/storage/s3_client.h"

#include <elio/elio.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace eliop2p;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; std::cout << "  [PASS] " << name << "\n"; }
    else      { ++g_fail; std::cout << "  [FAIL] " << name << "\n"; }
}

struct SimpleHttpResponse {
    int status = 0;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    std::string header(const std::string& k) const {
        auto it = headers.find(k);
        return it != headers.end() ? it->second : "";
    }
};

std::optional<SimpleHttpResponse> http_get(uint16_t port, const std::string& path,
                                           const std::string& extra_headers = "") {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return std::nullopt;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    timeval tv{15, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return std::nullopt;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
                      extra_headers + "Connection: close\r\n\r\n";
    if (send(sock, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
        close(sock);
        return std::nullopt;
    }
    std::string raw;
    char buf[65536];
    ssize_t n;
    size_t header_end = std::string::npos;
    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
        do { n = recv(sock, buf, sizeof(buf), 0); } while (n < 0 && errno == EINTR);
        if (n <= 0) { close(sock); return std::nullopt; }
        raw.append(buf, static_cast<size_t>(n));
    }
    SimpleHttpResponse resp;
    resp.status = std::stoi(raw.substr(raw.find(' ') + 1, raw.find("\r\n")));
    size_t pos = raw.find("\r\n") + 2;
    while (pos < header_end) {
        size_t eol = raw.find("\r\n", pos);
        std::string line = raw.substr(pos, eol - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());
            for (auto& c : key) c = static_cast<char>(std::tolower(c));
            resp.headers[key] = val;
        }
        pos = eol + 2;
    }
    size_t content_length = 0;
    if (auto it = resp.headers.find("content-length"); it != resp.headers.end()) {
        content_length = std::stoull(it->second);
    }
    resp.body.assign(raw.begin() + header_end + 4, raw.end());
    while (resp.body.size() < content_length) {
        do { n = recv(sock, buf, sizeof(buf), 0); } while (n < 0 && errno == EINTR);
        if (n <= 0) break;
        resp.body.insert(resp.body.end(), buf, buf + n);
    }
    close(sock);
    return resp;
}

struct TestNode {
    std::string name;
    P2PConfig p2p;
    ProxyConfig proxy_cfg;
    CacheConfig cache_cfg;
    StorageConfig storage_cfg;

    std::shared_ptr<elio::runtime::scheduler> scheduler;
    std::shared_ptr<ChunkManager> cache;
    std::shared_ptr<StorageClient> storage;
    std::unique_ptr<NodeDiscovery> discovery;
    std::shared_ptr<TransferManager> transfer;
    std::unique_ptr<ProxyServer> proxy;

    bool start(const std::string& node_id, const std::string& disk_path) {
        cache = std::make_shared<ChunkManager>(cache_cfg);
        cache->set_disk_cache_path(disk_path);
        cache->initialize_disk_cache();
        storage = StorageClientFactory::create(storage_cfg);
        discovery = std::make_unique<NodeDiscovery>(p2p);
        discovery->set_scheduler(scheduler);
        transfer = std::make_shared<TransferManager>(p2p);
        transfer->set_scheduler(scheduler);
        transfer->set_node_discovery(discovery.get());
        transfer->set_chunk_manager(cache.get());
        transfer->set_chunk_data_provider(
            [cm = cache](const std::string& chunk_id) -> std::shared_ptr<const std::vector<uint8_t>> {
                auto chunk = cm->get_chunk(chunk_id);
                if (!chunk) return nullptr;
                return std::shared_ptr<const std::vector<uint8_t>>(chunk, &chunk->data());
            });
        transfer->set_chunk_data_consumer(
            [cm = cache](const std::string& chunk_id, const std::vector<uint8_t>& data) {
                return cm->store_chunk(chunk_id, data);
            });
        proxy = std::make_unique<ProxyServer>(proxy_cfg, cache, storage);
        proxy->set_transfer_manager(transfer);
        if (!discovery->start()) return false;
        if (!transfer->start()) return false;
        proxy->set_transfer_manager(transfer);
        if (!proxy->start()) return false;

        PeerNode self;
        self.node_id = node_id;
        self.address = "127.0.0.1";
        self.port = p2p.listen_port;
        self.gossip_port = p2p.gossip_port;
        self.last_seen = static_cast<uint64_t>(std::time(nullptr));
        discovery->register_node(self);
        std::cout << name << " up: proxy=" << proxy_cfg.listen_port << "\n";
        return true;
    }

    void add_peer(uint16_t port, uint16_t gossip_port, const std::string& id) {
        PeerNode p;
        p.node_id = id;
        p.address = "127.0.0.1";
        p.port = port;
        p.gossip_port = gossip_port;
        p.last_seen = static_cast<uint64_t>(std::time(nullptr));
        discovery->add_peer(p);
    }

    void stop() {
        if (proxy) proxy->stop();
        if (transfer) transfer->stop();
        if (discovery) discovery->stop();
    }
};

}  // namespace

int main() {
    Logger::instance().set_level(elio::log::level::debug);

    auto scheduler = std::make_shared<elio::runtime::scheduler>(4);
    scheduler->start();

    StorageConfig minio;
    minio.type = "s3";
    minio.endpoint = "http://localhost:9090";
    minio.region = "us-east-1";
    minio.access_key = "minioadmin";
    minio.secret_key = "minioadmin123";
    minio.bucket = "test-bucket";

    TestNode b;
    b.name = "NodeB";
    b.scheduler = scheduler;
    b.p2p.listen_port = 19211;
    b.p2p.gossip_port = 19311;
    b.p2p.gossip_interval_sec = 1;
    b.proxy_cfg.listen_port = 18291;
    b.proxy_cfg.bind_address = "127.0.0.1";
    b.cache_cfg.memory_cache_size_mb = 128;
    b.cache_cfg.disk_cache_path = "";
    b.storage_cfg = minio;

    TestNode a;
    a.name = "NodeA";
    a.scheduler = scheduler;
    a.p2p.listen_port = 19212;
    a.p2p.gossip_port = 19312;
    a.p2p.gossip_interval_sec = 1;
    a.proxy_cfg.listen_port = 18290;
    a.proxy_cfg.bind_address = "127.0.0.1";
    a.cache_cfg.memory_cache_size_mb = 128;
    a.cache_cfg.disk_cache_path = "";
    a.storage_cfg = minio;

    std::cout << "=== Starting nodes (B pre-warms chunk0 only) ===\n";
    if (!b.start("node-b", "/tmp/e2e-partial-b")) return 1;
    if (!a.start("node-a", "/tmp/e2e-partial-a")) return 1;
    a.add_peer(19211, 19311, "node-b");
    b.add_peer(19212, 19312, "node-a");

    const std::string obj = "/test-bucket/photos/2024/large_40mb.bin";
    const std::string full_key = "test-bucket/photos/2024/large_40mb.bin";
    const uint64_t CS = 16ull * 1024 * 1024;  // chunk size

    // ---- 1. B pre-warms chunk0 only (1MB range read) -----------------------
    std::cout << "\n=== 1. B pre-warms chunk0 via 1MB range read ===\n";
    auto warm = http_get(18291, obj, "Range: bytes=0-1048575\r\n");
    check(warm && warm->status == 206, "B: pre-warm range read -> 206");
    check(warm && warm->header("x-chunk-sources") == "0:STORAGE",
          "B: chunk0 came from STORAGE (got '" +
          (warm ? warm->header("x-chunk-sources") : "") + "')");
    check(b.cache->has_chunk(full_key + "_0"), "B: holds chunk0");
    check(!b.cache->has_chunk(full_key + "_1"), "B: does NOT hold chunk1");
    check(!b.cache->has_chunk(full_key + "_2"), "B: does NOT hold chunk2");

    // let the chunk0 announce reach A
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // ---- 2. A reads a slice of chunk0 -> P2P --------------------------------
    std::cout << "\n=== 2. A reads slice of chunk0 (B has it) ===\n";
    auto r2 = http_get(18290, obj, "Range: bytes=0-99\r\n");
    check(r2 && r2->status == 206, "A: chunk0 slice -> 206");
    check(r2 && r2->header("x-chunk-sources") == "0:P2P",
          "A: chunk0 served via P2P from B (got '" +
          (r2 ? r2->header("x-chunk-sources") : "") + "')");

    // ---- 3. A reads a slice of chunk1 (nobody has it) -> STORAGE ------------
    std::cout << "\n=== 3. A reads slice of chunk1 (no peer has it) ===\n";
    std::string range1 = "Range: bytes=" + std::to_string(CS + 100) + "-" +
                         std::to_string(CS + 199) + "\r\n";
    auto r3 = http_get(18290, obj, range1);
    check(r3 && r3->status == 206, "A: chunk1 slice -> 206");
    check(r3 && r3->header("x-chunk-sources") == "1:STORAGE",
          "A: chunk1 backhauled from STORAGE (got '" +
          (r3 ? r3->header("x-chunk-sources") : "") + "')");

    // ---- 4. A reads range spanning chunk0+chunk1 ----------------------------
    std::cout << "\n=== 4. A reads range spanning chunk0+chunk1 ===\n";
    std::string range01 = "Range: bytes=" + std::to_string(CS - 512) + "-" +
                          std::to_string(CS + 511) + "\r\n";
    auto r4 = http_get(18290, obj, range01);
    check(r4 && r4->status == 206 && r4->body.size() == 1024, "A: spanning range -> 206, 1024B");
    check(r4 && r4->header("x-chunk-sources") == "0:CACHE,1:CACHE",
          "A: both chunks now local (got '" +
          (r4 ? r4->header("x-chunk-sources") : "") + "')");

    // On-demand granularity proof: A fetched ONLY what was asked for
    check(a.cache->has_chunk(full_key + "_0"), "A: holds chunk0");
    check(a.cache->has_chunk(full_key + "_1"), "A: holds chunk1");
    check(!a.cache->has_chunk(full_key + "_2"), "A: still does NOT hold chunk2 (no over-fetch)");

    // byte-exactness of the spanning range against the ORIGIN FILE on disk
    // (do NOT fetch a reference copy through B - that would warm B's cache
    // and invalidate the step-6 premise)
    std::vector<uint8_t> origin_data;
    {
        std::ifstream f("/tmp/testdata/photos/2024/large_40mb.bin", std::ios::binary);
        if (f) {
            origin_data.assign(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
        }
    }
    check(origin_data.size() == 40ull * 1024 * 1024,
          "reference: origin file readable (" + std::to_string(origin_data.size()) + " bytes)");
    if (origin_data.size() == 40ull * 1024 * 1024 && r4) {
        check(std::memcmp(r4->body.data(), origin_data.data() + (CS - 512), 1024) == 0,
              "A: spanning range bytes match origin exactly");
    }

    // ---- 5. A reads the FULL object: mixed-tier single response --------------
    std::cout << "\n=== 5. A reads FULL object (chunk2 must backhaul) ===\n";
    auto r5 = http_get(18290, obj);
    check(r5 && r5->status == 200 && r5->body.size() == 40ull * 1024 * 1024,
          "A: full object 200 + 40MB");
    check(r5 && r5->header("x-chunk-sources") == "0:CACHE,1:CACHE,2:STORAGE",
          "A: mixed tiers in ONE response (got '" +
          (r5 ? r5->header("x-chunk-sources") : "") + "')");
    if (r5 && r5->body.size() == origin_data.size()) {
        check(r5->body == origin_data, "A: assembled object byte-identical to origin");
    }

    // ---- 6. B reads a slice of chunk1 -> P2P from A ---------------------------
    std::cout << "\n=== 6. B reads chunk1 (A has it, B doesn't) ===\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // announce propagation
    auto r6 = http_get(18291, obj, range1);
    check(r6 && r6->status == 206, "B: chunk1 slice -> 206");
    check(r6 && r6->header("x-chunk-sources") == "1:P2P",
          "B: chunk1 pulled via P2P from A (got '" +
          (r6 ? r6->header("x-chunk-sources") : "") + "')");
    if (!origin_data.empty() && r6) {
        check(std::memcmp(r6->body.data(), origin_data.data() + (CS + 100), 100) == 0,
              "B: chunk1 bytes match origin");
    }

    // ---- Teardown --------------------------------------------------------------
    std::cout << "\n=== Stopping ===\n";
    a.stop();
    b.stop();
    scheduler->shutdown();

    std::cout << "\n========================================\n";
    std::cout << "E2E RESULT: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
