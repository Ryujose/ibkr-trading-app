# Helper tools, gated on the top-level IBKR_BUILD_TOOLS option.

# gen_tones — generates the 11 alert-tone WAVs into assets/sounds/tones/.
# Self-contained: no project dependencies, just C++ stdlib + filesystem.
add_executable(gen_tones gen_tones.cpp)
target_compile_features(gen_tones PRIVATE cxx_std_20)

# Some older GCC builds put std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(gen_tones PRIVATE stdc++fs)
endif()

# gen_voice.sh is shipped as-is — it's a shell wrapper around `piper` + `sox`,
# not a CMake target. See tools/README.md for prerequisites.
