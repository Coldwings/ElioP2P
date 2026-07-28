// End-to-end P2P distribution test against a real MinIO backend.
//
// Topology:
//   Node B (edge, warm): proxy :18281, chunk :19201, gossip :19301, storage -> MinIO :9090
//   Node A (edge, cold): proxy :18280, chunk :19202, gossip :19302, storage -> :19999 (DEAD)
//
// Node A's storage endpoint points at a dead port on purpose: any object it
// successfully serves can ONLY have come from the P2P network. That makes
// the P2P distribution claim hard-verifiable.
//
// Verified:
//   1. Multi-segment S3 keys route correctly (photos/2024/medium_20mb.bin)
//   2. Node B serves from storage, byte-identical to the origin (SHA256)
//   3. Node A serves the same object purely over P2P (dead backend), chunked
//   4. HTTP Range requests (intra-chunk, cross-chunk, suffix) return exact bytes
//   5. Small single-chunk object end to end
//
// Usage: ./bin/e2e_p2p_cluster
// Requires: MinIO on :9090 with test-bucket populated by test/e2e_setup.sh

#include "eliop2p/base/config.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/cache/chunk_manager.h"
#include "eliop2p/p2p/node_discovery.h"
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/proxy/server.h"
#include "eliop2p/storage/s3_client.h"

#include <elio/elio.hpp>
#include <elio/hash/sha256.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
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
    if (cond) {
        ++g_pass;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    auto digest = elio::hash::sha256(data.data(), data.size());
    return elio::hash::sha256_hex(digest);
}

// Minimal HTTP/1.1 client (Content-Length responses only, which is what the
// proxy produces)
struct SimpleHttpResponse {
    int status = 0;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

std::optional<SimpleHttpResponse> http_get(uint16_t port, const std::string& path,
                                           const std::string& extra_headers = "") {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return std::nullopt;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    timeval tv{10, 0};
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
    // Read until headers complete (retry on EINTR: the process runs
    // coroutine schedulers whose timers/signals may interrupt recv)
    size_t header_end = std::string::npos;
    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
        do {
            n = recv(sock, buf, sizeof(buf), 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            std::cerr << "http_get(" << path << "): header recv failed, n=" << n
                      << " errno=" << errno << " raw_size=" << raw.size() << "\n";
            close(sock);
            return std::nullopt;
        }
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
        do {
            n = recv(sock, buf, sizeof(buf), 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            std::cerr << "http_get(" << path << "): body recv stopped, n=" << n
                      << " errno=" << errno << " got=" << resp.body.size()
                      << " want=" << content_length << "\n";
            break;
        }
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
    // Optional override for the chunk provider (used by the malicious node
    // to serve poisoned bytes)
    TransferManager::ChunkDataProvider provider_override;

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
        if (provider_override) {
            transfer->set_chunk_data_provider(provider_override);
        } else {
            transfer->set_chunk_data_provider(
                [cm = cache](const std::string& chunk_id) -> std::shared_ptr<const std::vector<uint8_t>> {
                    auto chunk = cm->get_chunk(chunk_id);
                    if (!chunk) return nullptr;
                    return std::shared_ptr<const std::vector<uint8_t>>(chunk, &chunk->data());
                });
        }
        transfer->set_chunk_data_consumer(
            [cm = cache](const std::string& chunk_id, const std::vector<uint8_t>& data) {
                return cm->store_chunk(chunk_id, data);
            });

        proxy = std::make_unique<ProxyServer>(proxy_cfg, cache, storage);
        proxy->set_transfer_manager(transfer);

        if (!discovery->start()) {
            std::cerr << name << ": discovery start failed\n";
            return false;
        }
        if (!transfer->start()) {
            std::cerr << name << ": transfer start failed\n";
            return false;
        }
        proxy->set_transfer_manager(transfer);
        if (!proxy->start()) {
            std::cerr << name << ": proxy start failed\n";
            return false;
        }

        PeerNode self;
        self.node_id = node_id;
        self.address = "127.0.0.1";
        self.port = p2p.listen_port;
        self.gossip_port = p2p.gossip_port;
        self.last_seen = static_cast<uint64_t>(std::time(nullptr));
        discovery->register_node(self);

        std::cout << name << " started: proxy=" << proxy_cfg.listen_port
                  << " chunk=" << p2p.listen_port << " gossip=" << p2p.gossip_port << "\n";
        return true;
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

    // ---- Node B: warm edge, real storage ---------------------------------
    TestNode b;
    b.name = "NodeB";
    b.scheduler = scheduler;
    b.p2p.listen_port = 19201;
    b.p2p.gossip_port = 19301;
    b.p2p.gossip_interval_sec = 2;
    b.proxy_cfg.listen_port = 18281;
    b.proxy_cfg.bind_address = "127.0.0.1";
    b.cache_cfg.memory_cache_size_mb = 256;
    b.cache_cfg.disk_cache_path = "";
    b.storage_cfg.type = "s3";
    b.storage_cfg.endpoint = "http://localhost:9090";
    b.storage_cfg.region = "us-east-1";
    b.storage_cfg.access_key = "minioadmin";
    b.storage_cfg.secret_key = "minioadmin123";
    b.storage_cfg.bucket = "test-bucket";

    // ---- Node A: cold edge, DEAD storage (P2P-only) -----------------------
    TestNode a;
    a.name = "NodeA";
    a.scheduler = scheduler;
    a.p2p.listen_port = 19202;
    a.p2p.gossip_port = 19302;
    a.p2p.gossip_interval_sec = 2;
    a.proxy_cfg.listen_port = 18280;
    a.proxy_cfg.bind_address = "127.0.0.1";
    a.cache_cfg.memory_cache_size_mb = 256;
    a.cache_cfg.disk_cache_path = "";
    a.storage_cfg = b.storage_cfg;
    a.storage_cfg.endpoint = "http://localhost:19999";  // dead on purpose

    std::cout << "=== Starting nodes ===\n";
    if (!b.start("node-b", "/tmp/e2e-cache-b")) return 1;
    if (!a.start("node-a", "/tmp/e2e-cache-a")) return 1;

    // Interconnect via manual peer addition
    PeerNode a_info;
    a_info.node_id = "node-a";
    a_info.address = "127.0.0.1";
    a_info.port = 19202;
    a_info.gossip_port = 19302;
    a_info.last_seen = static_cast<uint64_t>(std::time(nullptr));

    PeerNode b_info;
    b_info.node_id = "node-b";
    b_info.address = "127.0.0.1";
    b_info.port = 19201;
    b_info.gossip_port = 19301;
    b_info.last_seen = static_cast<uint64_t>(std::time(nullptr));

    a.discovery->add_peer(b_info);
    b.discovery->add_peer(a_info);

    const std::string medium_obj = "/test-bucket/photos/2024/medium_20mb.bin";
    const std::string small_obj = "/test-bucket/small.txt";

    // ---- 1. Node B: multi-segment key from storage ------------------------
    std::cout << "\n=== 1. Node B serves multi-segment key from storage ===\n";
    auto t0 = std::chrono::steady_clock::now();
    auto rb = http_get(18281, medium_obj);
    auto t1 = std::chrono::steady_clock::now();
    check(rb.has_value(), "B: GET 20MB object returns a response");
    if (rb) {
        check(rb->status == 200, "B: status 200 (got " + std::to_string(rb->status) + ")");
        check(rb->body.size() == 20u * 1024 * 1024,
              "B: body is 20MB (" + std::to_string(rb->body.size()) + ")");
        check(sha256_hex(rb->body) == "0008307014bfe7f1fbe6a4c8dcf5f87b14326cc8711b88ff60f1f529abd32988",
              "B: SHA256 matches origin");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  [INFO] B storage fetch took " << ms << " ms\n";
    }

    // ---- 2. Wait for chunk announcements to reach Node A ------------------
    std::cout << "\n=== 2. Waiting for gossip chunk announcements ===\n";
    const std::string chunk0 = "test-bucket/photos/2024/medium_20mb.bin_0";
    bool announced = false;
    for (int i = 0; i < 50 && !announced; ++i) {
        auto peers = a.discovery->get_peers_with_chunk(chunk0);
        announced = !peers.empty();
        if (!announced) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(announced, "A learned via gossip that B holds chunk 0");

    // ---- 3. Node A serves the same object purely over P2P -----------------
    std::cout << "\n=== 3. Node A serves same object (backend is DEAD) ===\n";
    t0 = std::chrono::steady_clock::now();
    auto ra = http_get(18280, medium_obj);
    t1 = std::chrono::steady_clock::now();
    check(ra.has_value(), "A: GET 20MB object returns a response");
    if (ra) {
        check(ra->status == 200, "A: status 200 (got " + std::to_string(ra->status) + ")");
        check(ra->body.size() == 20u * 1024 * 1024,
              "A: body is 20MB (" + std::to_string(ra->body.size()) + ")");
        check(sha256_hex(ra->body) == "0008307014bfe7f1fbe6a4c8dcf5f87b14326cc8711b88ff60f1f529abd32988",
              "A: SHA256 identical to origin (P2P data intact)");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::cout << "  [INFO] A P2P fetch took " << ms << " ms\n";
    }

    // ---- 4. Range requests on Node A --------------------------------------
    std::cout << "\n=== 4. Range requests ===\n";
    // Intra-chunk: bytes 0-99 of chunk 0
    auto rr1 = http_get(18280, medium_obj, "Range: bytes=0-99\r\n");
    check(rr1 && rr1->status == 206, "Range 0-99 -> 206");
    if (rr1 && rr1->body.size() == 100 && rb) {
        check(std::memcmp(rr1->body.data(), rb->body.data(), 100) == 0,
              "Range 0-99 bytes match origin prefix");
    } else {
        check(false, "Range 0-99 returns 100 bytes");
    }
    // Cross-chunk boundary: 1MB before and after the 16MB boundary
    uint64_t boundary = 16ull * 1024 * 1024;
    std::string range_hdr = "Range: bytes=" + std::to_string(boundary - 1024) + "-" +
                            std::to_string(boundary + 1023) + "\r\n";
    auto rr2 = http_get(18280, medium_obj, range_hdr);
    check(rr2 && rr2->status == 206, "Cross-chunk range -> 206");
    if (rr2 && rr2->body.size() == 2048 && rb) {
        check(std::memcmp(rr2->body.data(), rb->body.data() + (boundary - 1024), 2048) == 0,
              "Cross-chunk bytes match origin");
    } else {
        check(false, "Cross-chunk range returns 2048 bytes");
    }
    // Suffix range: last 512 bytes
    auto rr3 = http_get(18280, medium_obj, "Range: bytes=-512\r\n");
    check(rr3 && rr3->status == 206 && rr3->body.size() == 512, "Suffix range -512 -> 206, 512 bytes");
    if (rr3 && rr3->body.size() == 512 && rb) {
        check(std::memcmp(rr3->body.data(), rb->body.data() + rb->body.size() - 512, 512) == 0,
              "Suffix bytes match origin tail");
    }

    // ---- 5. Small object end to end ---------------------------------------
    std::cout << "\n=== 5. Small single-chunk object ===\n";
    auto rs_b = http_get(18281, small_obj);
    check(rs_b && rs_b->status == 200 && rs_b->body.size() == 1024, "B: small.txt 1024 bytes");
    // let announce propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto rs_a = http_get(18280, small_obj);
    check(rs_a && rs_a->status == 200, "A: small.txt 200 via P2P");
    if (rs_a && rs_b) {
        check(rs_a->body == rs_b->body, "A: small.txt identical");
    }

    // ---- 6. Cache hit on repeat (A now has it locally) --------------------
    std::cout << "\n=== 6. Repeat fetch is a local cache hit ===\n";
    t0 = std::chrono::steady_clock::now();
    auto ra2 = http_get(18280, medium_obj);
    t1 = std::chrono::steady_clock::now();
    check(ra2 && ra2->status == 200 && ra2->body.size() == 20u * 1024 * 1024,
          "A: repeat fetch 200");
    auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "  [INFO] A repeat (local cache) fetch took " << ms2 << " ms\n";

    // ---- 7. Malicious peer poisoning attempt ------------------------------
    std::cout << "\n=== 7. Poisoned peer cannot win the race ===\n";
    // Node C serves WRONG bytes for chunk 0 (16MB of 0xEE) but with a
    // consistent self-reported hash - the classic poisoning attack. The
    // origin-anchored hash in A's meta must reject it; honest B still wins.
    TestNode c;
    c.name = "NodeC(evil)";
    c.scheduler = scheduler;
    c.p2p.listen_port = 19203;
    c.p2p.gossip_port = 19303;
    c.p2p.gossip_interval_sec = 2;
    c.proxy_cfg.listen_port = 18282;
    c.proxy_cfg.bind_address = "127.0.0.1";
    c.cache_cfg.memory_cache_size_mb = 64;
    c.cache_cfg.disk_cache_path = "";
    c.storage_cfg = b.storage_cfg;
    c.storage_cfg.endpoint = "http://localhost:19998";  // also dead
    c.provider_override =
        [chunk0_id = chunk0](const std::string& chunk_id) -> std::shared_ptr<const std::vector<uint8_t>> {
            if (chunk_id == chunk0_id) {
                return std::make_shared<const std::vector<uint8_t>>(16u * 1024 * 1024, 0xEE);
            }
            return nullptr;
        };
    if (!c.start("node-c", "/tmp/e2e-cache-c")) return 1;

    PeerNode c_info;
    c_info.node_id = "node-c";
    c_info.address = "127.0.0.1";
    c_info.port = 19203;
    c_info.gossip_port = 19303;
    c_info.last_seen = static_cast<uint64_t>(std::time(nullptr));
    a.discovery->add_peer(c_info);
    b.discovery->add_peer(c_info);
    c.discovery->add_peer(a_info);
    c.discovery->add_peer(b_info);

    // C claims it has chunk 0 (lies about content, but announce is legit)
    c.discovery->announce_chunk(chunk0);

    // A must forget chunk 0 so it is forced back onto the network
    a.cache->remove_chunk(chunk0);

    // Give the announce a moment to propagate
    bool c_known = false;
    for (int i = 0; i < 30 && !c_known; ++i) {
        auto peers = a.discovery->get_peers_with_chunk(chunk0);
        for (const auto& p : peers) {
            if (p.node_id == "node-c") { c_known = true; break; }
        }
        if (!c_known) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(c_known, "A learned that (evil) C claims chunk 0");

    // A fetches a slice covering chunk 0 only; the race includes C's
    // poisoned copy which MUST lose against the origin-anchored hash.
    auto rp = http_get(18280, medium_obj, "Range: bytes=0-99\r\n");
    check(rp && rp->status == 206, "A: range fetch with evil peer still 206");
    if (rp && rp->body.size() == 100 && rb) {
        check(std::memcmp(rp->body.data(), rb->body.data(), 100) == 0,
              "A: bytes are the origin's, not the poisoned copy");
    } else {
        check(false, "A: range fetch returns 100 bytes under attack");
    }

    // ---- Teardown ----------------------------------------------------------
    std::cout << "\n=== Stopping nodes ===\n";
    c.stop();
    a.stop();
    b.stop();
    scheduler->shutdown();

    std::cout << "\n========================================\n";
    std::cout << "E2E RESULT: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "========================================\n";
    return g_fail == 0 ? 0 : 1;
}
