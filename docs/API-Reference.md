# API Reference

Public headers live under `include/eliop2p/`. This is a signature inventory;
behavioral contracts are in [[API Contracts]].

## cache/chunk_manager.h

```cpp
class ChunkManager {
public:
    explicit ChunkManager(const CacheConfig& config);
    std::shared_ptr<const Chunk> get_chunk(const std::string& chunk_id);
    bool store_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data);
    bool store_chunk(const ChunkMetadata& metadata, const std::vector<uint8_t>& data);
    bool remove_chunk(const std::string& chunk_id);
    bool has_chunk(const std::string& chunk_id) const;
    std::optional<ChunkMetadata> get_metadata(const std::string& chunk_id) const;
    void update_metadata(const ChunkMetadata& metadata);

    static std::string compute_chunk_id(const std::string& object_key,
                                        uint64_t offset, uint64_t chunk_size);
    static std::string compute_sha256(const std::vector<uint8_t>& data);
    bool verify_chunk(const std::string& chunk_id, const std::vector<uint8_t>& data) const;

    CacheStats get_memory_cache_stats() const;
    CacheStats get_disk_cache_stats() const;
    uint64_t total_memory_usage() const;
    uint64_t total_disk_usage() const;

    void set_disk_cache_path(const std::string& path);
    bool initialize_disk_cache();
    bool persist_to_disk(const std::string& chunk_id, const std::vector<uint8_t>& data);
    std::optional<std::vector<uint8_t>> load_from_disk(const std::string& chunk_id) const;
    bool sync_metadata() const;

    bool should_promote_to_disk() const;
    bool should_demote_from_memory() const;
    bool promote_to_memory(const std::string& chunk_id);
    bool demote_to_disk(const std::string& chunk_id);
    void trigger_eviction();
};
```

## cache/lru_cache.h

```cpp
class LRUCache {
public:
    using EvictionCallback = std::function<void(const std::string&, const std::vector<uint8_t>&)>;
    explicit LRUCache(uint64_t max_capacity_bytes);
    LRUCache(uint64_t max_capacity_bytes, float w_time, float w_replica, float w_heat);

    std::shared_ptr<const Chunk> get(const std::string& key);   // nullptr on miss
    bool put(const std::string& key, const std::vector<uint8_t>& data);  // false if oversized
    bool remove(const std::string& key);
    bool exists(const std::string& key) const;
    void clear();
    void set_eviction_callback(EvictionCallback cb);
    CacheStats stats() const;
    void maybe_evict();
    void evict_to_target();
    float usage() const;
    uint64_t current_size() const;
    uint64_t max_capacity() const;
    void update_chunk_metadata(const std::string& key, uint32_t replica_count, HeatLevel heat);
    void set_heat_thresholds(uint32_t hot, uint32_t warm);
};
```

## p2p/transfer.h

```cpp
class TransferManager {
public:
    explicit TransferManager(const P2PConfig& config);

    bool start();                                     // spawns chunk TCP server
    void stop();

    elio::coro::task<std::optional<std::vector<uint8_t>>>
        download_chunk(const TransferRequest& request,
                       TransferProgressCallback progress_cb = nullptr);
    elio::coro::task<bool>
        download_chunk_to_file(const TransferRequest& request,
                               const std::string& dest_path,
                               TransferProgressCallback progress_cb = nullptr);
    elio::coro::task<bool>
        upload_chunk(const std::string& chunk_id,
                     const std::vector<uint8_t>& data,
                     const PeerNode& target_peer);
    void cancel_transfer(const std::string& chunk_id);

    std::vector<TransferProgress> get_active_transfers() const;
    TransferStats get_stats() const;
    PeerList select_k_peers(const PeerList& candidates, uint32_t k, TransferMode mode) const;

    void set_upload_limit(uint64_t mbps);             // 0 = unlimited
    void set_download_limit(uint64_t mbps);

    bool save_progress(const std::string& chunk_id, uint64_t downloaded_bytes);
    bool load_progress(const std::string& chunk_id, uint64_t& downloaded_bytes) const;
    bool has_progress(const std::string& chunk_id) const;

    void set_chunk_manager(ChunkManager* manager);
    void set_node_discovery(NodeDiscovery* discovery);
    void announce_local_chunk(const std::string& chunk_id);

    using ChunkDataProvider = std::function<std::shared_ptr<const std::vector<uint8_t>>(const std::string&)>;
    void set_chunk_data_provider(ChunkDataProvider provider);
    using ChunkDataConsumer = std::function<bool(const std::string&, const std::vector<uint8_t>&)>;
    void set_chunk_data_consumer(ChunkDataConsumer consumer);

    uint16_t get_listen_port() const;
    elio::coro::task<void> start_tcp_server();
    void stop_tcp_server();
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);
};

struct TransferRequest {
    std::string chunk_id;
    std::string object_key;
    uint64_t offset = 0;
    uint64_t expected_size = 0;
    TransferMode mode = TransferMode::FastestFirst;   // NearestFirst | FastestFirst | RarestFirst
    uint32_t k_value = 5;
    bool enable_resume = true;
    PeerList sources;                                 // empty → query discovery
    std::string expected_sha256;                      // origin-anchored verification
};
```

## p2p/node_discovery.h

```cpp
class NodeDiscovery {
public:
    explicit NodeDiscovery(const P2PConfig& config);

    bool start();
    void stop();
    bool register_node(const PeerNode& local_node);   // also spawns gossip listener
    void unregister_node();

    PeerList get_all_peers() const;
    PeerList get_peers_with_chunk(const std::string& chunk_id) const;
    void announce_chunk(const std::string& chunk_id);
    void remove_chunk_announcement(const std::string& chunk_id);

    void set_on_node_discovered(NodeDiscoveredCallback cb);
    void set_on_node_lost(NodeLostCallback cb);
    void add_peer(const PeerNode& peer);
    void remove_peer(const std::string& node_id);

    void start_gossip_protocol();
    void stop_gossip_protocol();
    elio::coro::task<void> gossip_tick();
    elio::coro::task<void> heartbeat_tick();

    void update_peer_latency(const std::string& node_id, double latency_ms);
    void update_peer_throughput(const std::string& node_id, double mbps);
    uint32_t get_chunk_rarity(const std::string& chunk_id) const;

    uint16_t get_listen_port() const;
    elio::coro::task<void> start_tcp_server();
    void stop_tcp_server();
    std::shared_ptr<elio::runtime::scheduler> get_scheduler() const;
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);
};

struct PeerNode {
    std::string node_id;
    std::string address;
    uint16_t port;                 // chunk transfer port
    uint16_t gossip_port = 0;      // 0 → same as port
    uint64_t last_seen;
    uint64_t available_memory_mb, available_disk_mb;
    double latency_ms = 0.0, throughput_mbps = 0.0;
    std::unordered_set<std::string> available_chunks;
    bool is_active() const;
};
```

## proxy/request_handler.h

```cpp
class RequestHandler {
public:
    RequestHandler(std::shared_ptr<ChunkManager> cache,
                   std::shared_ptr<StorageClient> storage);
    elio::coro::task<HttpResponse> handle_request(const HttpRequest& request);
    void set_transfer_manager(std::shared_ptr<TransferManager> tm);
    void set_p2p_fallback_enabled(bool enabled);
    void set_allowed_bucket(std::string bucket);

    struct ObjectInfo { uint64_t size; std::string etag; std::vector<std::string> chunk_hashes; };
    struct ByteRange { uint64_t start, end; bool valid; };
    static ByteRange parse_range_header(const std::string& header, uint64_t object_size);
    elio::coro::task<std::optional<ObjectInfo>> get_object_info(const CacheKeyInfo& info);
    elio::coro::task<std::pair<std::shared_ptr<const std::vector<uint8_t>>, ChunkSource>>
        fetch_chunk(const CacheKeyInfo& info, const ObjectInfo& obj, uint64_t chunk_index);
};
```

## proxy/server.h

```cpp
class ProxyServer {
public:
    ProxyServer(const ProxyConfig& config,
                std::shared_ptr<ChunkManager> cache,
                std::shared_ptr<StorageClient> storage);
    bool start();
    void stop();
    bool is_running() const;
    ProxyMetrics get_metrics() const;
    void set_transfer_manager(std::shared_ptr<TransferManager> tm);
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);
    void set_p2p_fallback_enabled(bool enabled);
};
```

## storage/s3_client.h

```cpp
class StorageClient {  // abstract
public:
    virtual bool test_connection() = 0;
    virtual elio::coro::task<std::optional<std::vector<std::string>>> list_buckets() = 0;
    virtual elio::coro::task<std::optional<ListObjectsResult>>
        list_objects(const std::string& bucket, const std::string& prefix = "",
                     const std::string& continuation_token = "", uint32_t max_keys = 1000) = 0;
    virtual elio::coro::task<std::optional<ObjectMetadata>>
        head_object(const std::string& bucket, const std::string& key) = 0;
    virtual elio::coro::task<std::optional<std::vector<uint8_t>>>
        get_object(const std::string& bucket, const std::string& key,
                   uint64_t offset = 0, uint64_t length = 0) = 0;
    virtual elio::coro::task<bool>
        put_object(const std::string& bucket, const std::string& key,
                   const std::vector<uint8_t>& data,
                   const std::string& content_type = "application/octet-stream") = 0;
    virtual elio::coro::task<bool> delete_object(const std::string& bucket, const std::string& key) = 0;
    virtual std::string generate_presigned_url(const std::string& bucket, const std::string& key,
                                               uint64_t expires_in_seconds = 3600) = 0;
    virtual elio::coro::task<bool> bucket_exists(const std::string& bucket) = 0;
    virtual void set_scheduler(std::shared_ptr<elio::runtime::scheduler>) = 0;  // no-op (ambient)
};

class StorageClientFactory {
public:
    static std::unique_ptr<StorageClient> create(const StorageConfig& config);
    static StorageBackend detect_backend(const std::string& endpoint);
};
```

## control/client.h, control/server.h

```cpp
class ControlPlaneClient {
public:
    bool connect();
    void disconnect();
    bool register_node(const NodeRegistration& node);
    elio::coro::task<bool> send_heartbeat(const NodeHeartbeat& hb);
    elio::coro::task<std::optional<std::vector<ChunkLocation>>>
        query_chunk_locations(const std::vector<std::string>& chunk_ids);
    elio::coro::task<std::optional<std::vector<NodeRegistration>>> query_nodes();
    elio::coro::task<std::optional<ControlPlaneMetrics>> get_metrics();
    elio::coro::task<std::optional<std::vector<ReplicationCommand>>> fetch_replication_commands();
    elio::coro::task<bool> ack_command(const std::string& command_id);
    void subscribe_to_updates(ChunkLocationsCallback cb);
    void subscribe_to_replication_commands(ReplicationCommandCallback cb);
    void start_heartbeat_loop(const std::function<NodeStatus()>& status_provider);
    void stop_heartbeat_loop();
};

class ControlPlaneServer {
public:
    explicit ControlPlaneServer(const ControlPlaneServerConfig& config);
    bool start();
    void stop();
    bool is_running() const;
    ControlPlaneServerMetrics get_metrics() const;
    size_t get_node_count() const;
    size_t get_active_node_count() const;
    std::vector<NodeRegistration> get_all_nodes() const;
    std::optional<std::vector<ChunkLocation>>
        query_chunk_locations(const std::vector<std::string>& chunk_ids) const;
    void set_scheduler(std::shared_ptr<elio::runtime::scheduler> scheduler);
};
```
