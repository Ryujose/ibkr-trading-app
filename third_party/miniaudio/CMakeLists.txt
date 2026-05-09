# miniaudio is a single-header library. Vendor it as an INTERFACE target so
# any CMake target can pull it in via target_link_libraries(... PRIVATE miniaudio).
#
# Source: https://github.com/mackron/miniaudio (pinned at v0.11.22 — 2025-02-24)
# Licence: public domain or MIT-0 (see header).

add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Per miniaudio's documentation, exactly one translation unit must compile the
# implementation. Consumers do this by writing `#define MINIAUDIO_IMPLEMENTATION`
# above their `#include "miniaudio.h"` — typically inside the .cpp that wraps
# its API. We do that in NotificationService.cpp.

# Platform-specific link requirements for the audio backends miniaudio uses.
if(UNIX AND NOT APPLE)
    # Linux backends: ALSA / PulseAudio / JACK are dlopen'd at runtime, so no
    # link-time dependency. Just need pthreads + libdl + libm.
    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
elseif(APPLE)
    # macOS uses Core Audio — needs CoreFoundation + CoreAudio + AudioToolbox + AudioUnit.
    target_link_libraries(miniaudio INTERFACE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit")
elseif(WIN32)
    # WASAPI / DirectSound / WinMM all link via these.
    target_link_libraries(miniaudio INTERFACE ole32 winmm)
endif()
