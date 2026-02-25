cmake_minimum_required(VERSION 3.20)
project(ElioP2P VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Enable cache
option(ENABLE_TESTING "Enable unit tests" ON)
if(ENABLE_TESTING)
    enable_testing()
endif()

# FetchContent for dependencies
include(FetchContent)

# Elio coroutine framework
message(STATUS "Fetching Elio coroutine framework...")
FetchContent_Declare(
    elio
    GIT_REPOSITORY https://github.com/Coldwings/Elio.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

# nlohmann_json for JSON serialization (header-only)
message(STATUS "Fetching nlohmann_json...")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# Set FetchContent options
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Set FetchContent options
set(ELIO_EXTERNAL ON CACHE BOOL "" FORCE)
set(ELIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ELIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_TLS ON CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(elio)

# After FetchContent_MakeAvailable, the Elio target should be available
# Create alias if needed for compatibility
if(TARGET elio AND NOT TARGET Elio::Elio)
    add_library(Elio::Elio ALIAS elio)
endif()

# Get Elio include directory from target property if available
if(TARGET Elio::Elio)
    get_target_property(ELIO_INCLUDE_DIR Elio::Elio INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT ELIO_INCLUDE_DIR)
        # Fallback to FetchContent directory
        set(ELIO_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/elio-src/include")
    endif()
else()
    # Fallback for when Elio is not available
    set(ELIO_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/elio-src/include")
    message(WARNING "Elio library not found - some features may not work")
endif()

# Get fmt include directory
if(TARGET fmt::fmt)
    get_target_property(FMT_INCLUDE_DIR fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT FMT_INCLUDE_DIR)
        set(FMT_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/fmt-src/include")
    endif()
else()
    set(FMT_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/fmt-src/include")
endif()

# Find OpenSSL for crypto operations
find_package(OpenSSL REQUIRED)

# Print configuration summary
message(STATUS "=== ElioP2P Configuration Summary ===")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
if(TARGET Elio::Elio)
    message(STATUS "  Elio: Found")
else()
    message(STATUS "  Elio: Not found")
endif()
message(STATUS "=======================================")

# Source directories
add_subdirectory(src)
add_subdirectory(test)

# Executable target
add_executable(${PROJECT_NAME} main.cpp)

# Include directories - must be after add_executable
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ELIO_INCLUDE_DIR}
    ${FMT_INCLUDE_DIR}
)

# Link OpenSSL for crypto operations
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Library target
add_library(${PROJECT_NAME}lib STATIC)
if(TARGET Elio::Elio)
    target_link_libraries(${PROJECT_NAME}lib PUBLIC Elio::Elio)
endif()
target_sources(${PROJECT_NAME}lib PRIVATE
    $<TARGET_OBJECTS:base_obj>
    $<TARGET_OBJECTS:cache_obj>
    $<TARGET_OBJECTS:p2p_obj>
    $<TARGET_OBJECTS:control_obj>
    $<TARGET_OBJECTS:proxy_obj>
    $<TARGET_OBJECTS:storage_obj>
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}lib OpenSSL::SSL OpenSSL::Crypto)
