#include "eliop2p/base/config.h"
#include "eliop2p/base/logger.h"
#include "eliop2p/storage/s3_client.h"
#include <elio/elio.hpp>
#include <elio/runtime/async_main.hpp>
#include <iostream>
using namespace eliop2p;
int main() {
    Logger::instance().set_level(elio::log::level::debug);
    StorageConfig cfg;
    cfg.type = "s3";
    cfg.endpoint = "http://localhost:9090";
    cfg.region = "us-east-1";
    cfg.access_key = "minioadmin";
    cfg.secret_key = "minioadmin123";
    auto client = StorageClientFactory::create(cfg);
    return elio::run([&client]() -> elio::coro::task<int> {
        auto meta = co_await client->head_object("test-bucket", "photos/2024/medium_20mb.bin");
        std::cout << (meta ? "HEAD OK" : "HEAD FAILED") << "\n";
        co_return meta ? 0 : 1;
    });
}
