cmake_minimum_required(VERSION 3.20)

project(ibkr-trading-app
    VERSION 1.0.0
    DESCRIPTION "Interactive Brokers Trading Application with C++ and ImGui/Vulkan"
    LANGUAGES C CXX
)

# ============================================================================
# C++ Standard Configuration (C++20 for modern features)
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization flags for production builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-O3" COMPILER_SUPPORTS_O3)
    if(COMPILER_SUPPORTS_O3)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
    endif()

    check_cxx_compiler_flag("-DNDEBUG" COMPILER_SUPPORTS_NDEBUG)
    if(COMPILER_SUPPORTS_NDEBUG)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG")
    endif()
endif()

# Platform-specific settings
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(APPLE)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Build Options
# ============================================================================
option(IBKR_BUILD_TESTS "Build unit tests" OFF)
option(IBKR_BUILD_DOCS "Generate documentation" OFF)
option(IBKR_USE_SYSTEM_IMGUI "Use system-installed ImGui" OFF)
option(IBKR_USE_SYSTEM_VULKAN "Use system-installed Vulkan headers" OFF)

# ============================================================================
# Dependency Validation and Fetching
# ============================================================================

# Check C++20 support
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-std=c++20" COMPILER_SUPPORTS_CXX20)
if(NOT COMPILER_SUPPORTS_CXX20)
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} does not support C++20. This is required.")
endif()

# Vulkan SDK detection
if(IBKR_USE_SYSTEM_VULKAN)
    find_package(Vulkan QUIET)
else()
    find_package(Vulkan REQUIRED)
endif()

if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found. The application requires Vulkan for rendering.")
    if(LINUX)
        message(STATUS "On Linux, install via: sudo apt install libvulkan-dev vulkan-tools")
    elseif(WIN32)
        message(STATUS "On Windows, download from https://www.lunarg.com/vulkan-sdk/")
    elseif(APPLE)
        message(STATUS "On macOS, Vulkan support requires MoltenVK setup")
    endif()
endif()

# ImGui - Fetch from GitHub if not available system-wide
if(NOT IBKR_USE_SYSTEM_IMGUI OR NOT EXISTS "${CMAKE_SOURCE_DIR}/external/imgui")
    include(FetchContent)

    message(STATUS "Fetching ImGui dependencies...")

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.6-docking
    )

    FetchContent_MakeAvailable(imgui)
else()
    message(STATUS "Using system ImGui at ${CMAKE_SOURCE_DIR}/external/imgui")
endif()

# ==============================================================================
# GML - Math library for vector operations
# ==============================================================================
include(FetchContent)

message(STATUS "Fetching glm (OpenGL Mathematics library)...")

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glm)

# ImPlot - Scientific plotting for ImGui
message(STATUS "Fetching implot (ImGui plotting library)...")

FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG v0.17
)

FetchContent_MakeAvailable(implot)

# ============================================================================
# Create ImGui Library from source files
# ============================================================================
file(GLOB IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/*.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_widgets.cpp
)

add_library(imgui-lib STATIC
    ${IMGUI_SOURCES}
)

# Add ImGui backend files for Vulkan and GLFW
file(GLOB IMGUI_BACKENDS_SOURCES
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui-lib PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_sources(imgui-lib PRIVATE
    ${IMGUI_BACKENDS_SOURCES}
)

# Create ImPlot Library from source files
file(GLOB IMPLOT_SOURCES
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

add_library(implot-lib STATIC
    ${IMPLOT_SOURCES}
)

target_include_directories(implot-lib PUBLIC
    ${implot_SOURCE_DIR}
)

# Link ImGui library to ImPlot (ImPlot requires ImGui)
target_link_libraries(implot-lib PRIVATE imgui-lib)
target_include_directories(implot-lib PUBLIC
    ${implot_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
)
# ============================================================================
# IB TWS C++ API — bundled in-tree at twsapi_macunix.1037.02
# ============================================================================
set(IBAPI_DIR "${CMAKE_SOURCE_DIR}/twsapi_macunix.1037.02/IBJts/source/cppclient/client")
set(IBAPI_PROTO_DIR "${IBAPI_DIR}/protobufUnix")

# BID stubs (replaces Intel libbid which is not available on this system)
add_library(bid-stubs STATIC src/bid_stubs/bid_stubs.c)
target_include_directories(bid-stubs PUBLIC ${IBAPI_DIR})

# Protobuf (system-installed)
find_package(Protobuf REQUIRED)

# IB API static library
file(GLOB IBAPI_CPP_SOURCES "${IBAPI_DIR}/*.cpp")
file(GLOB IBAPI_PROTO_SOURCES "${IBAPI_PROTO_DIR}/*.cc")

add_library(ibapi-lib STATIC ${IBAPI_CPP_SOURCES} ${IBAPI_PROTO_SOURCES})
target_include_directories(ibapi-lib PUBLIC
    ${IBAPI_DIR}
    ${IBAPI_PROTO_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(ibapi-lib PUBLIC bid-stubs ${Protobuf_LIBRARIES} pthread)
# Suppress warnings from third-party IB API code
target_compile_options(ibapi-lib PRIVATE -w)
# IB API uses C++14 features; allow its sources to compile as C++17
set_target_properties(ibapi-lib PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED OFF)

# ============================================================================
# Main Application Sources
# ============================================================================
set(APPLICATION_SOURCES
    src/main.cpp
    src/core/services/IBKRClient.cpp
    src/ui/windows/ChartWindow.cpp
    src/ui/windows/NewsWindow.cpp
    src/ui/windows/TradingWindow.cpp
    src/ui/windows/ScannerWindow.cpp
    src/ui/windows/PortfolioWindow.cpp
    src/ui/windows/OrdersWindow.cpp
)

# Find GLFW3 library (use system installed)
find_package(glfw3 REQUIRED)

set(APPLICATION_LIBS
    # Core libraries - our custom libs + system imports
    imgui-lib
    implot-lib
    Vulkan::Vulkan
    glm::glm
    glfw
    ibapi-lib
    bid-stubs
)

# Platform-specific libraries
if(WIN32)
    list(APPEND APPLICATION_LIBS ws2_32 wsock32 gdi32 shell32)
elseif(UNIX)
    if(APPLE)
        # macOS specific libraries
        find_library(COCOA_LIBRARY Cocoa)
        find_library(FOUNDATION_LIBRARY Foundation)
        find_library(IOKIT_LIBRARY IOKit)
        find_library(COREVIDEO_LIBRARY CoreVideo)
        find_library(METAL_LIBRARY Metal)
        find_library(METALKIT_LIBRARY MetalKit)

        if(COCOA_LIBRARY)
            list(APPEND APPLICATION_LIBS ${COCOA_LIBRARY})
        endif()
        if(FOUNDATION_LIBRARY)
            list(APPEND APPLICATION_LIBS ${FOUNDATION_LIBRARY})
        endif()
        if(IOKIT_LIBRARY)
            list(APPEND APPLICATION_LIBS ${IOKIT_LIBRARY})
        endif()
        if(COREVIDEO_LIBRARY)
            list(APPEND APPLICATION_LIBS ${COREVIDEO_LIBRARY})
        endif()
    else()
        # Linux/X11 specific libraries
        find_package(X11 REQUIRED)

        list(APPEND APPLICATION_LIBS dl pthread)  # dlopen, etc.
    endif()
endif()

# Create executable
add_executable(ibkr-trading-app ${APPLICATION_SOURCES})

# Include directories
target_include_directories(ibkr-trading-app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/examples
    ${implot_SOURCE_DIR}
    ${glm_SOURCE_DIR}
    ${IBAPI_DIR}
    ${IBAPI_PROTO_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# Include GLFW headers from fetch content
target_include_directories(ibkr-trading-app PRIVATE ${glfw_SOURCE_DIR}/include)

# Link libraries
target_link_libraries(ibkr-trading-app PRIVATE
    ${APPLICATION_LIBS}
)

# Compiler definitions
target_compile_definitions(ibkr-trading-app PRIVATE
    $<$<CONFIG:DEBUG>:DEBUG_MODE>
    $<$<CONFIG:RELEASE>:NDEBUG>
)

# ============================================================================
# Installation Rules
# ============================================================================
install(TARGETS ibkr-trading-app
    RUNTIME DESTINATION bin
)

# ============================================================================
# Optional: Tests
# ============================================================================
if(IBKR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# Build Summary
# ============================================================================
message(STATUS "")
message(STATUS "=======================================")
message(STATUS "ibkr-trading-app Configuration Summary")
message(STATUS "=======================================")
message(STATUS "Version:           ${PROJECT_VERSION}")
message(STATUS "Build Type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "Platform:          ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "Vulkan Found:      ${Vulkan_FOUND}")
message(STATUS "ImGui Source:      ${imgui_SOURCE_DIR}")
message(STATUS "ImPlot Source:     ${implot_SOURCE_DIR}")
message(STATUS "GLM Source:        ${glm_SOURCE_DIR}")
message(STATUS "IB API Dir:        ${IBAPI_DIR}")
message(STATUS "Protobuf Found:    ${Protobuf_FOUND}")
message(STATUS "")
message(STATUS "To build, run:")
message(STATUS "  cmake --build <build-dir>")
message(STATUS "=======================================")
message(STATUS "")
