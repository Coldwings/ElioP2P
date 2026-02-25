# Test directory CMakeLists.txt

if(ENABLE_TESTING)
    enable_testing()

    # Add include directories
    include_directories(${CMAKE_SOURCE_DIR}/include)

    # Add cache test executable
    add_executable(test_cache test_cache.cpp)
    target_link_libraries(test_cache PRIVATE ElioP2Plib)
    add_test(NAME CacheTest COMMAND test_cache)

    # Add gossip test executable
    add_executable(test_gossip test_gossip.cpp)
    target_link_libraries(test_gossip PRIVATE ElioP2Plib)
    add_test(NAME GossipTest COMMAND test_gossip)

    # Add transfer test executable
    add_executable(test_transfer test_transfer.cpp)
    target_link_libraries(test_transfer PRIVATE ElioP2Plib)
    add_test(NAME TransferTest COMMAND test_transfer)

    # Add storage test executable
    add_executable(test_storage test_storage.cpp)
    target_link_libraries(test_storage PRIVATE ElioP2Plib OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME StorageTest COMMAND test_storage)

    message(STATUS "Testing enabled")
endif()
