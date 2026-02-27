# Test directory CMakeLists.txt

if(ENABLE_TESTING)
    enable_testing()

    # Add include directories
    include_directories(${CMAKE_SOURCE_DIR}/include)

    # Enable Catch2 tests
    include(Catch)

    # Add cache test executable
    add_executable(test_cache test_cache.cpp)
    target_link_libraries(test_cache PRIVATE ElioP2Plib Catch2::Catch2WithMain)
    catch_discover_tests(test_cache)

    # Add gossip test executable
    add_executable(test_gossip test_gossip.cpp)
    target_link_libraries(test_gossip PRIVATE ElioP2Plib Catch2::Catch2WithMain)
    catch_discover_tests(test_gossip)

    # Add transfer test executable
    add_executable(test_transfer test_transfer.cpp)
    target_link_libraries(test_transfer PRIVATE ElioP2Plib Catch2::Catch2WithMain)
    catch_discover_tests(test_transfer)

    # Add storage test executable
    add_executable(test_storage test_storage.cpp)
    target_link_libraries(test_storage PRIVATE ElioP2Plib OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    catch_discover_tests(test_storage)

    # Add integration test executable
    add_executable(test_integration test_integration.cpp)
    target_link_libraries(test_integration PRIVATE ElioP2Plib OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    catch_discover_tests(test_integration)

    # Add functional test executable
    add_executable(test_functional test_functional.cpp)
    target_link_libraries(test_functional PRIVATE ElioP2Plib OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    catch_discover_tests(test_functional)

    # Add E2E test executable
    add_executable(test_e2e test_e2e.cpp)
    target_link_libraries(test_e2e PRIVATE ElioP2Plib OpenSSL::SSL OpenSSL::Crypto Catch2::Catch2WithMain)
    catch_discover_tests(test_e2e)

    message(STATUS "Testing enabled with Catch2")
endif()
