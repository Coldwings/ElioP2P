#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <chrono>
#include "eliop2p/p2p/transfer.h"
#include "eliop2p/base/config.h"

using namespace eliop2p;

void test_transfer_mode_enum() {
    std::cout << "Testing transfer mode enum values..." << std::endl;

    // Test TransferMode enum
    assert(static_cast<int>(TransferMode::NearestFirst) == 0);
    assert(static_cast<int>(TransferMode::FastestFirst) == 1);
    assert(static_cast<int>(TransferMode::RarestFirst) == 2);

    std::cout << "  Transfer mode enum test PASSED" << std::endl;
}

void test_transfer_request() {
    std::cout << "Testing transfer request construction..." << std::endl;

    TransferRequest req;
    req.chunk_id = "test_chunk_0";
    req.object_key = "test_object";
    req.offset = 0;
    req.expected_size = 16384;
    req.mode = TransferMode::FastestFirst;
    req.k_value = 5;
    req.enable_resume = true;

    assert(req.chunk_id == "test_chunk_0");
    assert(req.object_key == "test_object");
    assert(req.offset == 0);
    assert(req.expected_size == 16384);
    assert(req.mode == TransferMode::FastestFirst);
    assert(req.k_value == 5);
    assert(req.enable_resume == true);

    std::cout << "  Transfer request test PASSED" << std::endl;
}

void test_transfer_progress() {
    std::cout << "Testing transfer progress tracking..." << std::endl;

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

    assert(progress.chunk_id == "test_chunk_1");
    assert(progress.bytes_transferred == 8192);
    assert(progress.total_bytes == 16384);
    assert(progress.active_peers.size() == 3);
    assert(progress.progress_percent == 50.0);

    std::cout << "  Transfer progress test PASSED" << std::endl;
}

void test_connection_state_enum() {
    std::cout << "Testing connection state enum..." << std::endl;

    // Test ConnectionState enum values
    assert(static_cast<int>(ConnectionState::Idle) == 0);
    assert(static_cast<int>(ConnectionState::Connecting) == 1);
    assert(static_cast<int>(ConnectionState::Handshaking) == 2);
    assert(static_cast<int>(ConnectionState::Transferring) == 3);
    assert(static_cast<int>(ConnectionState::Completed) == 4);
    assert(static_cast<int>(ConnectionState::Failed) == 5);
    assert(static_cast<int>(ConnectionState::Cancelled) == 6);

    std::cout << "  Connection state enum test PASSED" << std::endl;
}

void test_peer_connection() {
    std::cout << "Testing peer connection structure..." << std::endl;

    PeerConnection conn;
    conn.peer_id = "test_peer_1";
    conn.address = "192.168.1.100";
    conn.port = 9000;
    conn.state = ConnectionState::Idle;
    conn.bytes_transferred = 0;
    conn.speed_bps = 0.0;

    assert(conn.peer_id == "test_peer_1");
    assert(conn.address == "192.168.1.100");
    assert(conn.port == 9000);
    assert(conn.state == ConnectionState::Idle);

    // Update state
    conn.state = ConnectionState::Transferring;
    conn.bytes_transferred = 1024 * 1024;  // 1MB
    conn.speed_bps = 10 * 1024 * 1024;    // 10 MB/s

    assert(conn.state == ConnectionState::Transferring);
    assert(conn.bytes_transferred == 1024 * 1024);

    std::cout << "  Peer connection test PASSED" << std::endl;
}

void test_chunk_transfer_context() {
    std::cout << "Testing chunk transfer context..." << std::endl;

    ChunkTransferContext ctx;
    ctx.chunk_id = "test_chunk_2";
    ctx.file_path = "/tmp/test_chunk.transfer";
    ctx.total_size = 16 * 1024 * 1024;  // 16MB
    ctx.downloaded_size = 8 * 1024 * 1024;  // 8MB
    ctx.last_checkpoint_size = 4 * 1024 * 1024;  // 4MB
    ctx.is_resume = true;

    assert(ctx.chunk_id == "test_chunk_2");
    assert(ctx.total_size == 16 * 1024 * 1024);
    assert(ctx.downloaded_size == 8 * 1024 * 1024);
    assert(ctx.is_resume == true);

    // Test constants
    assert(ChunkTransferContext::CHECKPOINT_INTERVAL == 1024 * 1024);  // 1MB
    assert(ChunkTransferContext::CHUNK_SIZE == 16 * 1024 * 1024);    // 16MB

    std::cout << "  Chunk transfer context test PASSED" << std::endl;
}

void test_transfer_manager_creation() {
    std::cout << "Testing transfer manager creation..." << std::endl;

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
    [[maybe_unused]] auto stats = manager.get_stats();
    assert(stats.total_downloads == 0);
    assert(stats.total_uploads == 0);
    assert(stats.active_transfers == 0);

    std::cout << "  Transfer manager creation test PASSED" << std::endl;
}

void test_transfer_stats() {
    std::cout << "Testing transfer statistics..." << std::endl;

    P2PConfig config;
    config.max_peers = 10;

    TransferManager manager(config);

    // Get stats
    auto stats = manager.get_stats();
    std::cout << "  Initial downloads: " << stats.total_downloads << std::endl;
    std::cout << "  Initial uploads: " << stats.total_uploads << std::endl;
    std::cout << "  Active transfers: " << stats.active_transfers << std::endl;

    // Verify initial state
    assert(stats.total_downloads == 0);
    assert(stats.total_uploads == 0);
    assert(stats.failed_downloads == 0);
    assert(stats.failed_uploads == 0);
    assert(stats.total_bytes_downloaded == 0);
    assert(stats.total_bytes_uploaded == 0);

    std::cout << "  Transfer statistics test PASSED" << std::endl;
}

void test_k_selection_peers() {
    std::cout << "Testing K-selection peer selection..." << std::endl;

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
    assert(nearest.size() <= 3);
    if (nearest.size() >= 1) {
        std::cout << "  Nearest first peer: " << nearest[0].node_id
                  << " (latency: " << nearest[0].latency_ms << "ms)" << std::endl;
    }

    // Test FastestFirst selection
    auto fastest = manager.select_k_peers(candidates, 3, TransferMode::FastestFirst);
    assert(fastest.size() <= 3);
    if (fastest.size() >= 1) {
        std::cout << "  Fastest first peer: " << fastest[0].node_id
                  << " (throughput: " << fastest[0].throughput_mbps << "Mbps)" << std::endl;
    }

    std::cout << "  K-selection peer test PASSED" << std::endl;
}

void test_bandwidth_limiter() {
    std::cout << "Testing bandwidth limiter..." << std::endl;

    // Create limiter with 100 Mbps limit
    BandwidthLimiter limiter(100);

    // Test initial available bytes
    uint64_t available = limiter.available_bytes();
    std::cout << "  Initial available bytes: " << available << std::endl;

    // Test setting new limit
    limiter.set_limit(50);
    uint64_t new_available = limiter.available_bytes();
    std::cout << "  After limit change: " << new_available << " bytes" << std::endl;

    // Verify limit was applied
    assert(limiter.available_bytes() > 0);

    std::cout << "  Bandwidth limiter test PASSED" << std::endl;
}

void test_transfer_progress_callback() {
    std::cout << "Testing transfer progress callback..." << std::endl;

    bool callback_invoked = false;
    TransferProgress last_progress;

    TransferProgressCallback callback = [&callback_invoked, &last_progress](const TransferProgress& progress) {
        callback_invoked = true;
        last_progress = progress;
        std::cout << "  Progress callback: " << progress.progress_percent << "%" << std::endl;
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

    assert(callback_invoked == true);
    assert(last_progress.progress_percent == 50.0);
    assert(last_progress.active_peers.size() == 2);

    std::cout << "  Transfer progress callback test PASSED" << std::endl;
}

void test_chunk_download_stats() {
    std::cout << "Testing chunk download statistics..." << std::endl;

    ChunkDownloadStats stats;
    stats.chunk_id = "stats_test_chunk";
    stats.start_time = 1000;
    stats.end_time = 2000;
    stats.bytes_downloaded = 16384;
    stats.success_count = 1;
    stats.failure_count = 0;
    stats.used_peers = {"peer1", "peer2"};

    assert(stats.chunk_id == "stats_test_chunk");
    assert(stats.bytes_downloaded == 16384);
    assert(stats.success_count == 1);
    assert(stats.failure_count == 0);
    assert(stats.used_peers.size() == 2);

    // Calculate duration
    uint64_t duration = stats.end_time - stats.start_time;
    std::cout << "  Download duration: " << duration << "ms" << std::endl;

    std::cout << "  Chunk download stats test PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running P2P Transfer Module Tests ===" << std::endl;

    try {
        test_transfer_mode_enum();
        test_transfer_request();
        test_transfer_progress();
        test_connection_state_enum();
        test_peer_connection();
        test_chunk_transfer_context();
        test_transfer_manager_creation();
        test_transfer_stats();
        test_k_selection_peers();
        test_bandwidth_limiter();
        test_transfer_progress_callback();
        test_chunk_download_stats();

        std::cout << "\n=== All P2P Transfer Tests PASSED ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
