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

# Use static CRT on Windows to match x64-windows-static vcpkg triplet (/MT)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
option(IBKR_BUILD_TOOLS "Build helper tools (sound generators, etc.)" OFF)
option(IBKR_BUILD_DOCS "Generate documentation" OFF)
option(IBKR_USE_SYSTEM_IMGUI "Use system-installed ImGui" OFF)
option(IBKR_USE_SYSTEM_VULKAN "Use system-installed Vulkan headers" OFF)

# ============================================================================
# Dependency Validation and Fetching
# ============================================================================

# Check C++20 support
include(CheckCXXCompilerFlag)
if(MSVC)
    check_cxx_compiler_flag("/std:c++20" COMPILER_SUPPORTS_CXX20)
else()
    check_cxx_compiler_flag("-std=c++20" COMPILER_SUPPORTS_CXX20)
endif()
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

# Find GLFW3 early — needed by imgui backends
find_package(glfw3 REQUIRED)

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
target_link_libraries(imgui-lib PRIVATE glfw Vulkan::Vulkan)

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

# Protobuf — config-mode preferred (vcpkg/modern installs), fallback to module-mode
find_package(protobuf CONFIG QUIET)
if(protobuf_FOUND)
    # Config-mode found (vcpkg). Target protobuf::libprotobuf already exists with
    # transitive abseil deps. Include dirs propagate via link; no need for explicit vars.
    set(Protobuf_INCLUDE_DIRS "")
    set(Protobuf_FOUND TRUE)
else()
    find_package(Protobuf REQUIRED)
    # Module-mode: ensure modern target name exists for uniform usage below
    if(NOT TARGET protobuf::libprotobuf)
        add_library(protobuf::libprotobuf INTERFACE IMPORTED)
        target_link_libraries(protobuf::libprotobuf INTERFACE ${Protobuf_LIBRARIES})
        target_include_directories(protobuf::libprotobuf INTERFACE ${Protobuf_INCLUDE_DIRS})
    endif()
endif()

# IB API static library
file(GLOB IBAPI_CPP_SOURCES "${IBAPI_DIR}/*.cpp")
file(GLOB IBAPI_PROTO_SOURCES "${IBAPI_PROTO_DIR}/*.cc")

add_library(ibapi-lib STATIC ${IBAPI_CPP_SOURCES} ${IBAPI_PROTO_SOURCES})
target_include_directories(ibapi-lib PUBLIC
    ${IBAPI_DIR}
    ${IBAPI_PROTO_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
if(WIN32)
    target_link_libraries(ibapi-lib PUBLIC bid-stubs protobuf::libprotobuf)
else()
    target_link_libraries(ibapi-lib PUBLIC bid-stubs protobuf::libprotobuf pthread)
endif()
# Suppress warnings from third-party IB API code
if(MSVC)
    target_compile_options(ibapi-lib PRIVATE /w)
else()
    target_compile_options(ibapi-lib PRIVATE -w)
endif()
# IB API uses C++14 features; allow its sources to compile as C++17
set_target_properties(ibapi-lib PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED OFF)

# ============================================================================
# miniaudio — vendored single-header audio library (NotificationService)
# ============================================================================
add_subdirectory(third_party/miniaudio)

# ============================================================================
# Main Application Sources
# ============================================================================
set(APPLICATION_SOURCES
    src/main.cpp
    src/core/services/IBKRClient.cpp
    src/core/services/NotificationService.cpp
    src/ui/NotificationOverlay.cpp
    src/ui/windows/NotificationsWindow.cpp
    src/ui/windows/ChartWindow.cpp
    src/ui/windows/NewsWindow.cpp
    src/ui/windows/TradingWindow.cpp
    src/ui/windows/ScannerWindow.cpp
    src/ui/windows/PortfolioWindow.cpp
    src/ui/windows/OrdersWindow.cpp
    src/ui/windows/WatchlistWindow.cpp
    src/ui/windows/WshCalendarWindow.cpp
    src/ui/windows/ReplayWindow.cpp
)

set(APPLICATION_LIBS
    # Core libraries - our custom libs + system imports
    imgui-lib
    implot-lib
    Vulkan::Vulkan
    glm::glm
    glfw
    ibapi-lib
    bid-stubs
    miniaudio
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
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>  # prevent Windows.h min/max macros clashing with std::min/max
)

# ============================================================================
# macOS: bundle MoltenVK + Vulkan loader next to the binary
# ============================================================================
# macOS has no native Vulkan — apps go through MoltenVK (Metal translation
# layer). To keep the shipped binary self-contained, we copy libvulkan.1.dylib
# (the loader) and libMoltenVK.dylib (the ICD) next to the executable, ship a
# custom MoltenVK_icd.json with a relative library_path, and set
# VK_ICD_FILENAMES at startup (main.cpp) to point at it. INSTALL_RPATH is
# @executable_path so the binary finds the bundled loader.
if(APPLE)
    find_library(MOLTENVK_DYLIB
        NAMES MoltenVK
        PATHS /opt/homebrew/opt/molten-vk/lib
              /usr/local/opt/molten-vk/lib
              ENV VULKAN_SDK
        PATH_SUFFIXES lib macOS/lib
    )
    if(NOT MOLTENVK_DYLIB)
        message(FATAL_ERROR
            "libMoltenVK.dylib not found. Install with:  brew install molten-vk")
    endif()

    set_target_properties(ibkr-trading-app PROPERTIES
        INSTALL_RPATH "@executable_path"
        BUILD_WITH_INSTALL_RPATH FALSE
    )

    # Dev convenience: copy dylibs + ICD JSON next to the build-tree binary
    # so `./build/ibkr-trading-app` runs without `cmake --install`.
    add_custom_command(TARGET ibkr-trading-app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Vulkan_LIBRARY}"   "$<TARGET_FILE_DIR:ibkr-trading-app>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MOLTENVK_DYLIB}"   "$<TARGET_FILE_DIR:ibkr-trading-app>/"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:ibkr-trading-app>/vulkan/icd.d"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/cmake/MoltenVK_icd.json"
            "$<TARGET_FILE_DIR:ibkr-trading-app>/vulkan/icd.d/"
        VERBATIM
    )
endif()

# ============================================================================
# Installation Rules
# ============================================================================
# Install layout (used by `cmake --install build --prefix dist` and by the
# CI release pipeline). Binary and assets/ sit as siblings at the root of
# the install prefix so the runtime resolver hits <exe>/assets/sounds on
# its first candidate path — no PATH/CWD juggling required.
install(TARGETS ibkr-trading-app
    RUNTIME DESTINATION .
)
install(DIRECTORY assets DESTINATION .)

if(APPLE)
    # FOLLOW_SYMLINK_CHAIN is not a keyword on install(FILES) — it only works
    # with file(INSTALL ...). brew's libvulkan.1.dylib / libMoltenVK.dylib are
    # symlinks pointing into the Cellar (e.g. libvulkan.1.dylib →
    # libvulkan.1.4.341.dylib), so we need the chain followed at install time.
    install(CODE "
        file(INSTALL
            \"${Vulkan_LIBRARY}\"
            \"${MOLTENVK_DYLIB}\"
            DESTINATION \"\${CMAKE_INSTALL_PREFIX}\"
            FOLLOW_SYMLINK_CHAIN
        )
    ")
    install(FILES "${CMAKE_SOURCE_DIR}/cmake/MoltenVK_icd.json"
        DESTINATION vulkan/icd.d
    )
    # Defensive: brew's vulkan-loader normally ships with install_name
    # `@rpath/libvulkan.1.dylib`, but rewrite the binary's loader reference
    # and the bundled dylibs' ids if any of them ended up with absolute
    # Cellar paths. install_name_tool exits non-zero on a no-op, so each
    # call is wrapped in execute_process with ERROR_QUIET.
    install(CODE [[
        foreach(_lib libvulkan.1.dylib libMoltenVK.dylib)
            set(_path "${CMAKE_INSTALL_PREFIX}/${_lib}")
            if(EXISTS "${_path}")
                execute_process(
                    COMMAND install_name_tool -id "@rpath/${_lib}" "${_path}"
                    OUTPUT_QUIET ERROR_QUIET)
            endif()
        endforeach()
        set(_exe "${CMAKE_INSTALL_PREFIX}/ibkr-trading-app")
        if(EXISTS "${_exe}")
            execute_process(
                COMMAND otool -L "${_exe}"
                OUTPUT_VARIABLE _otool
                ERROR_QUIET)
            string(REGEX MATCHALL "[^ \t\r\n(]+libvulkan[^ \t\r\n(]*\\.dylib"
                _matches "${_otool}")
            foreach(_old IN LISTS _matches)
                if(NOT _old MATCHES "^@")
                    execute_process(
                        COMMAND install_name_tool
                            -change "${_old}" "@rpath/libvulkan.1.dylib" "${_exe}"
                        OUTPUT_QUIET ERROR_QUIET)
                endif()
            endforeach()
        endif()
    ]])
endif()

# ============================================================================
# Optional: Tests
# ============================================================================
if(IBKR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================================
# Optional: Helper Tools (gen_tones, etc.)
# ============================================================================
if(IBKR_BUILD_TOOLS)
    add_subdirectory(tools)
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
