# ============================================================================
# Test suite — Catch2 v3
# ============================================================================

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# Make the Catch CMake helpers (catch_discover_tests) available.
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(CTest)
include(Catch)

# ── Option: enable ASan + UBSan on tests-core ────────────────────────────────
option(IBKR_SANITIZE "Enable AddressSanitizer + UBSanitizer on tests-core" OFF)

# ============================================================================
# tests-core — pure logic; no IB API dependency
# ============================================================================
add_executable(tests-core
    test_market_data.cpp
    test_models.cpp
    test_ibkr_utils.cpp
    test_chart_analysis.cpp
)

target_include_directories(tests-core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(tests-core PRIVATE Catch2::Catch2WithMain)
target_compile_features(tests-core PRIVATE cxx_std_20)

if(IBKR_SANITIZE AND NOT MSVC)
    target_compile_options(tests-core PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(tests-core PRIVATE
        -fsanitize=address,undefined
    )
endif()

catch_discover_tests(tests-core)

# ============================================================================
# tests-ibkr — IBKRClient component tests; links ibapi-lib
# ============================================================================
add_executable(tests-ibkr
    test_ibkr_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/core/services/IBKRClient.cpp
)

target_include_directories(tests-ibkr PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IBAPI_DIR}
    ${IBAPI_PROTO_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(tests-ibkr PRIVATE
    Catch2::Catch2WithMain
    ibapi-lib
    bid-stubs
)

if(UNIX AND NOT APPLE)
    target_link_libraries(tests-ibkr PRIVATE pthread dl)
endif()

target_compile_features(tests-ibkr PRIVATE cxx_std_20)

# Suppress third-party warnings in the IB client sources when included here.
if(MSVC)
    target_compile_options(tests-ibkr PRIVATE /wd4244 /wd4267)
else()
    target_compile_options(tests-ibkr PRIVATE -Wno-unused-parameter)
endif()

catch_discover_tests(tests-ibkr)
