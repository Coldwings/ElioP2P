# Base components
add_library(base_obj OBJECT)
target_sources(base_obj PRIVATE
    base/logger.cpp
    base/config.cpp
    base/error_code.cpp
)
target_include_directories(base_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)
if(TARGET Elio::Elio)
    target_link_libraries(base_obj PUBLIC Elio::Elio)
endif()

# Cache management
add_library(cache_obj OBJECT)
target_sources(cache_obj PRIVATE
    cache/lru_cache.cpp
    cache/chunk_manager.cpp
)
target_include_directories(cache_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)
if(TARGET Elio::Elio)
    target_link_libraries(cache_obj PUBLIC Elio::Elio)
endif()

# P2P transport
add_library(p2p_obj OBJECT)
target_sources(p2p_obj PRIVATE
    p2p/node_discovery.cpp
    p2p/transfer.cpp
)
target_include_directories(p2p_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)
if(TARGET Elio::Elio)
    target_link_libraries(p2p_obj PUBLIC Elio::Elio)
endif()

# Control plane client
add_library(control_obj OBJECT)
target_sources(control_obj PRIVATE
    control/client.cpp
    control/metadata.cpp
)

target_include_directories(control_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include
)
if(TARGET Elio::Elio)
    target_link_libraries(control_obj PUBLIC Elio::Elio)
endif()
# Link OpenSSL for crypto operations (used by TLS in HTTP client)
target_link_libraries(control_obj PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# HTTP proxy
add_library(proxy_obj OBJECT)
target_sources(proxy_obj PRIVATE
    proxy/server.cpp
    proxy/request_handler.cpp
)
target_include_directories(proxy_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)
if(TARGET Elio::Elio)
    target_link_libraries(proxy_obj PUBLIC Elio::Elio)
endif()

# Object storage client
add_library(storage_obj OBJECT)
target_sources(storage_obj PRIVATE
    storage/s3_client.cpp
    storage/oss_client.cpp
)
target_include_directories(storage_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)
if(TARGET Elio::Elio)
    target_link_libraries(storage_obj PUBLIC Elio::Elio)
endif()
# Link OpenSSL for crypto operations
target_link_libraries(storage_obj PUBLIC OpenSSL::SSL OpenSSL::Crypto)
# Add URLEncode header-only library (header in include directory)
