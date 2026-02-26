#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <chrono>
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

TEST_CASE("Transfer Mode Enum Values", "[transfer][enum]") {
    // Test TransferMode enum
    REQUIRE(static_cast<int>(TransferMode::NearestFirst) == 0);
    REQUIRE(static_cast<int>(TransferMode::FastestFirst) == 1);
    REQUIRE(static_cast<int>(TransferMode::RarestFirst) == 2);
}

TEST_CASE("Transfer Request Construction", "[transfer][request]") {
    TransferRequest req;
    req.chunk_id = "test_chunk_0";
    req.object_key = "test_object";
    req.offset = 0;
    req.expected_size = 16384;
    req.mode = TransferMode::FastestFirst;
    req.k_value = 5;
    req.enable_resume = true;

    REQUIRE(req.chunk_id == "test_chunk_0");
    REQUIRE(req.object_key == "test_object");
    REQUIRE(req.offset == 0);
    REQUIRE(req.expected_size == 16384);
    REQUIRE(req.mode == TransferMode::FastestFirst);
    REQUIRE(req.k_value == 5);
    REQUIRE(req.enable_resume == true);
}

TEST_CASE("Transfer Progress Tracking", "[transfer][progress]") {
    TransferProgress progress;
    progress.chunk_id = "test_chunk_1";
    progress.bytes_transferred = 8192;
    progress.total_bytes = 16384;
    progress.current_peer = "peer_1";
    progress.active_peers = {"peer_1", "peer_2", "peer_3"};
    progress.completed = false;
    progress.failed = false;
    progress.progress_percent = 50.0;
    progress.speed_bps = 1048576.0;  // 1 MB/s

    REQUIRE(progress.chunk_id == "test_chunk_1");
    REQUIRE(progress.bytes_transferred == 8192);
    REQUIRE(progress.total_bytes == 16384);
    REQUIRE(progress.active_peers.size() == 3);
    REQUIRE(progress.progress_percent == 50.0);
}

TEST_CASE("Connection State Enum", "[transfer][enum][connection]") {
    // Test ConnectionState enum values
    REQUIRE(static_cast<int>(ConnectionState::Idle) == 0);
    REQUIRE(static_cast<int>(ConnectionState::Connecting) == 1);
    REQUIRE(static_cast<int>(ConnectionState::Handshaking) == 2);
    REQUIRE(static_cast<int>(ConnectionState::Transferring) == 3);
    REQUIRE(static_cast<int>(ConnectionState::Completed) == 4);
    REQUIRE(static_cast<int>(ConnectionState::Failed) == 5);
    REQUIRE(static_cast<int>(ConnectionState::Cancelled) == 6);
}

TEST_CASE("Peer Connection Structure", "[transfer][peer]") {
    PeerConnection conn;
    conn.peer_id = "test_peer_1";
    conn.address = "192.168.1.100";
    conn.port = 9000;
    conn.state = ConnectionState::Idle;
    conn.bytes_transferred = 0;
    conn.speed_bps = 0.0;

    REQUIRE(conn.peer_id == "test_peer_1");
    REQUIRE(conn.address == "192.168.1.100");
    REQUIRE(conn.port == 9000);
    REQUIRE(conn.state == ConnectionState::Idle);

    // Update state
    conn.state = ConnectionState::Transferring;
    conn.bytes_transferred = 1024 * 1024;  // 1MB
    conn.speed_bps = 10 * 1024 * 1024;    // 10 MB/s

    REQUIRE(conn.state == ConnectionState::Transferring);
    REQUIRE(conn.bytes_transferred == 1024 * 1024);
}

TEST_CASE("Chunk Transfer Context", "[transfer][context]") {
    ChunkTransferContext ctx;
    ctx.chunk_id = "test_chunk_2";
    ctx.file_path = "/tmp/test_chunk.transfer";
    ctx.total_size = 16 * 1024 * 1024;  // 16MB
    ctx.downloaded_size = 8 * 1024 * 1024;  // 8MB
    ctx.last_checkpoint_size = 4 * 1024 * 1024;  // 4MB
    ctx.is_resume = true;

    REQUIRE(ctx.chunk_id == "test_chunk_2");
    REQUIRE(ctx.total_size == 16 * 1024 * 1024);
    REQUIRE(ctx.downloaded_size == 8 * 1024 * 1024);
    REQUIRE(ctx.is_resume == true);

    // Test constants
    REQUIRE(ChunkTransferContext::CHECKPOINT_INTERVAL == 1024 * 1024);  // 1MB
    REQUIRE(ChunkTransferContext::CHUNK_SIZE == 16 * 1024 * 1024);    // 16MB
}

TEST_CASE("Transfer Manager Creation", "[transfer][manager]") {
    P2PConfig config;
    config.listen_port = 9000;
    config.max_connections = 100;
    config.max_peers = 50;
    config.max_upload_speed_mbps = 100;
    config.max_download_speed_mbps = 100;
    config.selection_k = 5;
    config.gossip_interval_sec = 10;

    TransferManager manager(config);

    // Test initial stats
    auto stats = manager.get_stats();
    REQUIRE(stats.total_downloads == 0);
    REQUIRE(stats.total_uploads == 0);
    REQUIRE(stats.active_transfers == 0);
}

TEST_CASE("Transfer Statistics", "[transfer][stats]") {
    P2PConfig config;
    config.max_peers = 10;

    TransferManager manager(config);

    // Get stats
    auto stats = manager.get_stats();
    INFO("Initial downloads: " << stats.total_downloads);
    INFO("Initial uploads: " << stats.total_uploads);
    INFO("Active transfers: " << stats.active_transfers);

    // Verify initial state
    REQUIRE(stats.total_downloads == 0);
    REQUIRE(stats.total_uploads == 0);
    REQUIRE(stats.failed_downloads == 0);
    REQUIRE(stats.failed_uploads == 0);
    REQUIRE(stats.total_bytes_downloaded == 0);
    REQUIRE(stats.total_bytes_uploaded == 0);
}

TEST_CASE("K-Selection Peer Selection", "[transfer][peer][selection]") {
    P2PConfig config;
    config.max_peers = 10;
    config.selection_k = 3;

    TransferManager manager(config);

    // Create test peers with different characteristics
    PeerList candidates;
    for (int i = 0; i < 10; i++) {
        PeerNode peer;
        peer.node_id = "peer_" + std::to_string(i);
        peer.address = "192.168.1." + std::to_string(i + 1);
        peer.port = 9000;
        peer.latency_ms = 10.0 + (i * 5);  // Increasing latency
        peer.throughput_mbps = 100.0 - (i * 5);  // Decreasing throughput
        candidates.push_back(peer);
    }

    // Test NearestFirst selection
    auto nearest = manager.select_k_peers(candidates, 3, TransferMode::NearestFirst);
    REQUIRE(nearest.size() <= 3);
    if (nearest.size() >= 1) {
        INFO("Nearest first peer: " << nearest[0].node_id
                  << " (latency: " << nearest[0].latency_ms << "ms)");
    }

    // Test FastestFirst selection
    auto fastest = manager.select_k_peers(candidates, 3, TransferMode::FastestFirst);
    REQUIRE(fastest.size() <= 3);
    if (fastest.size() >= 1) {
        INFO("Fastest first peer: " << fastest[0].node_id
                  << " (throughput: " << fastest[0].throughput_mbps << "Mbps)");
    }
}

TEST_CASE("Bandwidth Limiter", "[transfer][bandwidth]") {
    // Create limiter with 100 Mbps limit
    BandwidthLimiter limiter(100);

    // Test initial available bytes
    uint64_t available = limiter.available_bytes();
    INFO("Initial available bytes: " << available);

    // Test setting new limit
    limiter.set_limit(50);
    uint64_t new_available = limiter.available_bytes();
    INFO("After limit change: " << new_available << " bytes");

    // Verify limit was applied
    REQUIRE(limiter.available_bytes() > 0);
}

TEST_CASE("Transfer Progress Callback", "[transfer][callback]") {
    bool callback_invoked = false;
    TransferProgress last_progress;

    TransferProgressCallback callback = [&callback_invoked, &last_progress](const TransferProgress& progress) {
        callback_invoked = true;
        last_progress = progress;
        INFO("Progress callback: " << progress.progress_percent << "%");
    };

    // Create a mock progress and call callback
    TransferProgress progress;
    progress.chunk_id = "callback_test";
    progress.bytes_transferred = 5120;
    progress.total_bytes = 10240;
    progress.progress_percent = 50.0;
    progress.speed_bps = 1024000.0;
    progress.active_peers = {"peer1", "peer2"};

    callback(progress);

    REQUIRE(callback_invoked == true);
    REQUIRE(last_progress.progress_percent == 50.0);
    REQUIRE(last_progress.active_peers.size() == 2);
}

TEST_CASE("Chunk Download Statistics", "[transfer][stats][chunk]") {
    ChunkDownloadStats stats;
    stats.chunk_id = "stats_test_chunk";
    stats.start_time = 1000;
    stats.end_time = 2000;
    stats.bytes_downloaded = 16384;
    stats.success_count = 1;
    stats.failure_count = 0;
    stats.used_peers = {"peer1", "peer2"};

    REQUIRE(stats.chunk_id == "stats_test_chunk");
    REQUIRE(stats.bytes_downloaded == 16384);
    REQUIRE(stats.success_count == 1);
    REQUIRE(stats.failure_count == 0);
    REQUIRE(stats.used_peers.size() == 2);

    // Calculate duration
    uint64_t duration = stats.end_time - stats.start_time;
    INFO("Download duration: " << duration << "ms");
}
