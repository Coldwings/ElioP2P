# Development

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencies (FetchContent, pinned in `CMakeLists.txt`): Elio (main),
nlohmann_json, Catch2, CLI11, fmt (via Elio), OpenSSL.

> **Note**: `GIT_TAG main` for Elio makes builds non-reproducible. Pin to a
> commit hash or tag before any release.

Useful toggles:

| Option | Default | Effect |
|--------|---------|--------|
| `ENABLE_TESTING` | ON | builds ctest suite |
| `ELIO_ENABLE_HTTP` | ON (forced) | Elio HTTP server/client |
| `ELIO_ENABLE_TLS` | ON (forced) | TLS for storage endpoints |

## Code map

```
main.cpp                        application wiring & lifecycle
include/eliop2p/
  base/{config,logger,error_code}.h
  cache/{lru_cache,chunk_manager}.h
  p2p/{node_discovery,transfer}.h
  proxy/{server,request_handler}.h
  storage/{s3_client,oss_client}.h
  control/{client,server,metadata}.h
src/                            mirrors include/, one .cpp per header
test/
  test_*.cpp                    Catch2 unit tests (ctest)
  e2e_p2p_cluster.cpp           2-node distribution + poisoning attack E2E
  e2e_p2p_partial.cpp           on-demand partial-read E2E
  sig_probe.cpp, head_probe.cpp signature debugging utilities
```

Layer rule of thumb: **proxy → cache/P2P/storage adapters → Elio**. Nothing
in cache or p2p includes proxy headers; the proxy depends on everything.

## Conventions

- Coroutines are lazy `elio::coro::task<T>`; spawn with
  `scheduler->spawn(elio::coro::detail::task_access::release(std::move(t)))`
  for fire-and-forget, `scheduler->go_joinable(f, args...)` (with `std::ref`
  for references) when you need the result.
- Coroutine parameters are **by value** whenever the task may outlive the
  caller's stack frame — a spawned coroutine referencing a caller local is
  a use-after-free by definition.
- Every wire field is network byte order (`header_to_wire` /
  `header_from_wire`). Never send packed structs raw.
- No blocking syscalls on scheduler workers; wrap in `elio::spawn_blocking`.
- New public behavior needs a contract row in `docs/API-Contracts.md`.

## Pitfalls discovered the hard way (keep these in mind)

1. A `task` dropped without spawn/await is destroyed unstarted — servers
   "not listening" are usually this.
2. `joinable()` stays true until `join()`; a `while (joinable())` loop is
   an infinite detach factory, not a wait.
3. Closing a listener fd does **not** interrupt a pending io_uring accept —
   use the self-connect wakeup (see `stop_tcp_server*`).
4. Cross-scheduler `co_await join_handle` works, but the *spawned* task's
   scheduler must be running, or the join never completes.
5. Gossip dedup needs a content component in the message id, not just
   (source, timestamp, type).

## CI

GitHub Actions builds on GCC with `-Wall`, runs ctest. E2E tools are manual
(see [[Testing]]); run them before merging data-plane changes.
