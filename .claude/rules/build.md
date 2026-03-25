# Build Rules

## Commands

```bash
# Configure (default Release)
cmake -B build -S .

# Build
cmake --build build -j$(nproc)

# Run
DISPLAY=:1 ./build/ibkr-trading-app

# Debug build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Clean build
rm -rf build && cmake -B build -S . && cmake --build build
```

## Test Commands

```bash
# Configure with tests enabled
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DIBKR_BUILD_TESTS=ON

# Build (includes test binaries)
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run only a specific test binary
ctest --test-dir build -R "^tests-core" --output-on-failure
ctest --test-dir build -R "^tests-ibkr" --output-on-failure

# Configure with sanitizers (ASan + UBSan on tests-core only)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DIBKR_BUILD_TESTS=ON -DIBKR_SANITIZE=ON
cmake --build build --target tests-core -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build/tests/tests-core
```

## Binary Location
- Output: `build/ibkr-trading-app`
- Display `:1` is available on this machine — always use `DISPLAY=:1` when running

## Platform Notes
- **Linux**: Requires `vulkan-sdk` and development headers via package manager
- **macOS**: Requires Xcode CLI tools + Homebrew (`imgui`, `glm`)
- **Windows**: MSVC or MinGW; CMake handles Vulkan/ImGui deps automatically

## Dependencies (auto-fetched by CMake)
- `imgui` v1.92.6-docking (FetchContent)
- `implot` v0.17 (FetchContent)
- `glm` 1.0.1 (FetchContent)
- `glfw3` (system)
- `Vulkan` (system)
- `libprotobuf` 3.21.12 (system, `find_package(Protobuf REQUIRED)`)
- `ibapi-lib` — in-tree static lib from `twsapi_macunix.1037.02/...`
- `bid-stubs` — in-tree: `src/bid_stubs/bid_stubs.c` (double bit-cast for Intel BID64)
- `Catch2` v3.7.1 (FetchContent, only when `IBKR_BUILD_TESTS=ON`)

## CMake Options
- `IBKR_BUILD_TESTS` (default OFF) — build test binaries and register with CTest
- `IBKR_SANITIZE` (default OFF) — enable ASan + UBSan on `tests-core` (requires `IBKR_BUILD_TESTS=ON`, GCC/Clang only)

## CMake Notes
- Project languages must be `C CXX` (required for `bid_stubs.c`)
