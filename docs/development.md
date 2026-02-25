# ElioP2P Development Guide

## Project Structure

```
ElioP2P/
├── CMakeLists.txt          # Build configuration
├── main.cpp               # Application entry point
├── include/eliop2p/       # Public headers
│   ├── base/              # Logger, Config, ErrorCode
│   ├── cache/             # LRU Cache, Chunk Manager
│   ├── p2p/               # Node Discovery, Transfer
│   ├── control/           # Control Plane Client
│   ├── proxy/             # HTTP Proxy Server
│   └── storage/           # S3/OSS Client
├── src/                   # Implementation
│   ├── base/
│   ├── cache/
│   ├── p2p/
│   ├── control/
│   ├── proxy/
│   └── storage/
└── test/                  # Unit tests
```

## Building

### Prerequisites

- C++20 compatible compiler (GCC 11+, Clang 14+)
- CMake 3.20+
- OpenSSL
- liburing (optional)

### Build Steps

```bash
# Clone and setup
mkdir build && cd build
cmake ..

# Build
make -j$(nproc)

# Run tests
ctest

# Run application
./bin/ElioP2P
```

### Build Options

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake -DELIO_ENABLE_HTTP=ON ..
cmake -DELIO_ENABLE_TLS=ON ..
```

## Code Style

### Naming Conventions

- **Classes**: PascalCase (e.g., `ChunkManager`)
- **Functions**: PascalCase (e.g., `GetChunk`)
- **Variables**: camelCase (e.g., `chunkId`)
- **Constants**: kPascalCase (e.g., `kChunkSize`)
- **Files**: snake_case (e.g., `chunk_manager.cpp`)

### Headers

```cpp
#ifndef ELIOP2P_MODULE_COMPONENT_H
#define ELIOP2P_MODULE_COMPONENT_H

// Headers
#include <vector>
#include <string>

// Forward declarations
class ChunkManager;

namespace eliop2p {

// Code

} // namespace eliop2p

#endif // ELIOP2P_MODULE_COMPONENT_H
```

### Implementation

```cpp
#include "eliop2p/module/component.h"

namespace eliop2p {

// Implementation

} // namespace eliop2p
```

## Module Development

### Adding a New Module

1. Create header in `include/eliop2p/<module>/`
2. Create implementation in `src/<module>/`
3. Add to `src/CMakeLists.txt`
4. Add tests in `test/`

Example:
```cmake
# src/CMakeLists.txt
add_library(new_module_obj OBJECT)
target_sources(new_module_obj PRIVATE
    new_module/new_module.cpp
)
target_include_directories(new_module_obj PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
```

### Adding a Test

1. Create test file in `test/`
2. Add to `test/CMakeLists.txt`

```cmake
add_executable(test_new_module test_new_module.cpp)
target_link_libraries(test_new_module PRIVATE ElioP2Plib)
add_test(NAME NewModuleTest COMMAND test_new_module)
```

## Testing

### Running Tests

```bash
# All tests
ctest

# Specific test
./bin/test_cache

# With verbose output
ctest -V
```

### Writing Tests

```cpp
#include <cassert>
#include <iostream>

void test_basic_operation() {
    // Arrange
    MyClass obj;

    // Act
    obj.doSomething();

    // Assert
    assert(obj.getResult() == expected);
}

int main() {
    try {
        test_basic_operation();
        std::cout << "All tests passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
}
```

## Debugging

### Enable Debug Logging

```bash
ELIOP2P_LOG_LEVEL=DEBUG ./bin/ElioP2P
```

### Using GDB

```bash
gdb ./bin/ElioP2P
(gdb) run
(gdb) bt
```

## Performance

### Profiling

```bash
# CPU profiling
perf record -g ./bin/ElioP2P
perf report

# Memory profiling
valgrind --tool=massif ./bin/ElioP2P
```

### Benchmarks

```bash
# Run benchmark
./bin/benchmark
```

## Contributing

### Commit Guidelines

- Use English for commit messages
- Describe what and why, not how
- Keep commits atomic

### Code Review

- All changes require review
- Address feedback promptly
- Ensure tests pass
