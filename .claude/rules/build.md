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

## CMake Notes
- Project languages must be `C CXX` (required for `bid_stubs.c`)
