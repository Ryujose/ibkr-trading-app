# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Interactive Brokers Trading Application built with:
- **Language**: Modern C++ (latest stable, targeting C++20/23 features)
- **UI Framework**: Dear ImGui with Vulkan renderer
- **Build System**: CMake (cross-platform: Windows, Linux, macOS)
- **Trading API**: Interactive Brokers IB Gateway / TWS API

## Architecture

The application follows clean architecture principles with clear separation of concerns:

```
├── src/                    # Source code root
│   ├── core/             # Core abstractions and interfaces
│   │   ├── services/     # Service layer (IB Gateway integration)
│   │   └── models/       # Data models and entities
│   ├── ui/              # UI layer
│   │   ├── imgui/       # ImGui window implementations
│   │   └── docking/     # Docking system logic
│   ├── app/             # Application orchestration
│   └── main.cpp         # Entry point
├── CMakeLists.txt       # Main CMake configuration
└── build/               # Build directory (generated)
```

### Key Design Principles

1. **Service Layer Pattern**: IB Gateway communication abstracted behind interfaces
2. **Event-Driven Architecture**: Components communicate via event system for decoupling
3. **Dependency Injection**: Services injected into consumers for testability
4. **Cross-Platform Abstraction**: Platform-specific code isolated in adapters

## Common Commands

### Build and Run

```bash
# Configure (default Release build)
cmake -B build -S .

# Build
cmake --build build -j$(nproc)

# Run application
./build/bin/ibkr-trading-app

# Debug build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Clean build
rm -rf build && cmake -B build -S . && cmake --build build
```

### Platform-Specific

- **Windows**: Uses MSVC or MinGW, CMake handles Vulkan/ImGui dependencies automatically
- **Linux**: Requires vulkan-sdk and development headers installed via package manager
- **macOS**: Requires Xcode command line tools, Homebrew for dependencies (imgui, glm)

## Task Status

### Phase 1: Infrastructure
- [x] Set up CMake configuration with dependency validation (Task #1)
- [x] Build IMGUI base with dockable window system (Task #2)

### Phase 2: Core Features
- [x] Build mandatory login window for paper and live accounts (Task #3)
- [x] Build candlestick chart window with indicators (Task #4)

### Phase 3: Data Feeds
- [x] Build news window (portfolio, market, stock) (Task #5)
- [x] Build trading window (order book and execution) (Task #6)

### Phase 4: Advanced Features
- [x] Build market scanner for stocks and indexes (Task #7)
- [x] Build portfolio dashboard and account data window (Task #8)

## Important Notes

1. **IB Gateway Integration**: Requires running IB Gateway or TWS with API enabled on port 4001/7497
2. **Vulkan Installation**: Ensure Vulkan SDK is installed and ICD loaders are configured
3. **Cross-Platform Considerations**: Line endings, file paths, and library linking differ per platform
4. **Paper vs Live Accounts**: Connection settings differ (ports 7496/4002 for paper, 7497/4001 for live)
