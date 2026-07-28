// Quick signature probe: GET (not HEAD) against MinIO so the 403 body
// (which carries MinIO's expected StringToSign) is visible.
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
    cfg.bucket = "test-bucket";

    auto client = StorageClientFactory::create(cfg);

    // elio::run takes the callable, not the task
    return elio::run([&client]() -> elio::coro::task<int> {
        auto data = co_await client->get_object("test-bucket", "small.txt");
        if (data) {
            std::cout << "GET OK: " << data->size() << " bytes\n";
            co_return 0;
        }
        std::cout << "GET FAILED\n";
        co_return 1;
    });
}
