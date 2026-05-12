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

## Tools Commands (sound asset generators)

```bash
# Configure with helper tools enabled
cmake -B build -S . -DIBKR_BUILD_TOOLS=ON

# Build the alert-tone WAV generator
cmake --build build --target gen_tones -j$(nproc)

# Generate the 11 alert tones (skips existing files)
./build/tools/gen_tones assets/sounds/tones/
# Force-overwrite all 11
./build/tools/gen_tones --force assets/sounds/tones/

# Generate the 11 voice phrases (requires piper + sox + downloaded model)
# See tools/README.md for piper/model setup.
./tools/gen_voice.sh
./tools/gen_voice.sh --force
```

Tone WAVs are committed to `assets/sounds/tones/`. Voice WAVs land in
`assets/sounds/voice/` and are also committed once generated. Piper voice
models in `tools/piper-voices/` are gitignored — each developer downloads
their own copy.

## Binary Location
- Output: `build/ibkr-trading-app`
- Display `:1` is available on this machine — always use `DISPLAY=:1` when running

## Install / Shipping Layout

`cmake --install build --prefix <dir>` produces a self-contained directory:

```
<prefix>/
├── ibkr-trading-app        (binary at root, not under bin/)
└── assets/
    └── sounds/{tones,voice}/*.wav
```

Binary and `assets/` sit as siblings so the runtime resolver in `main.cpp`
hits its **first** candidate path (`<exeDir>/assets/sounds`) without
PATH/CWD juggling. The CI release pipeline (`.github/workflows/build.yml`)
runs `cmake --install build --prefix dist` after the test suite passes,
then uploads `dist/` as the `ibkr-trading-app-{linux,macos,windows}`
artifact — users download a single zip and run the binary in place with
all sounds working.

Driven by two install rules at `CMakeLists.txt:342`:
- `install(TARGETS ibkr-trading-app RUNTIME DESTINATION .)`
- `install(DIRECTORY assets DESTINATION .)`

The Windows install command needs `--config Release` because MSVC is a
multi-config generator (`cmake --install build --config Release --prefix dist`).

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
- `miniaudio` v0.11.22 — in-tree single header at `third_party/miniaudio/miniaudio.h` (audio backend for `NotificationService`; public domain / MIT-0)
- `Catch2` v3.7.1 (FetchContent, only when `IBKR_BUILD_TESTS=ON`)

## CMake Options
- `IBKR_BUILD_TESTS` (default OFF) — build test binaries and register with CTest
- `IBKR_BUILD_TOOLS` (default OFF) — build helper tools (e.g. the `gen_tones` sound generator under `tools/`)
- `IBKR_SANITIZE` (default OFF) — enable ASan + UBSan on `tests-core` (requires `IBKR_BUILD_TESTS=ON`, GCC/Clang only)

## CMake Notes
- Project languages must be `C CXX` (required for `bid_stubs.c`)
