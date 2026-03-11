cmake_minimum_required(VERSION 3.0)
project( IBKR LANGUAGES CXX )
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# For some reason cmake/ninja checks whether the system headers have changed, causing the entire project to rebuild each time
# This patch seems to fix it, ripped from https://stackoverflow.com/questions/47087237/cmake-and-ninja-rebuild-unnecessary-files
if (CMAKE_GENERATOR MATCHES "Ninja")
    file(WRITE "${CMAKE_BINARY_DIR}/CLANGMakeRulesOverwrite.cmake"
            "STRING(REPLACE \"-MD\" \"-MMD\" CMAKE_DEPFILE_FLAGS_C \"\${CMAKE_DEPFILE_FLAGS_C}\")\n"
            "STRING(REPLACE \"-MD\" \"-MMD\" CMAKE_DEPFILE_FLAGS_CXX \"\${CMAKE_DEPFILE_FLAGS_CXX}\")\n"
    )
    set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_BINARY_DIR}/CLANGMakeRulesOverwrite.cmake" CACHE INTERNAL "")
endif ()

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_GLIBCXX_DEBUG")
    set(CMAKE_DEBUG_POSTFIX d)
    message(STATUS "Setting libCXX debug flag")
endif()

option(ENABLE_LINTER "Run linter" OFF)
option(ENABLE_LINTER_ERROR "Run linter with errors" OFF)
if(${ENABLE_LINTER})
    find_program(TIDY NAMES clang-tidy-10 clang-tidy)
    if(${TIDY} STREQUAL "TIDY-NOTFOUND")
        message(FATAL_ERROR "clang-tidy not found! Necessary for linting!")
    else()
        message(STATUS "Found clang-tidy in ${TIDY}")
    endif()
    if(${ENABLE_LINTER_ERROR})
        set(CMAKE_CXX_CLANG_TIDY 
            ${TIDY};
            --warnings-as-errors=*;
            --checks=readability-*,-readability-magic-numbers,performance-*,portability-*,modernize-*,-modernize-use-trailing-return-type,-cppcoreguidelines-*;
        )
        set(CMAKE_C_CLANG_TIDY 
            ${TIDY};
            --warnings-as-errors=*;
            --checks=readability-*,-readability-magic-numbers,performance-*,portability-*,-clang-analyzer-security.insecureAPI.*;
        )
        add_compile_options(-Werror)
    else()
        set(CMAKE_CXX_CLANG_TIDY 
            ${TIDY};
            --checks=readability-*,-readability-magic-numbers,performance-*,portability-*,modernize-*,-modernize-use-trailing-return-type,-cppcoreguidelines-*;
        )
        set(CMAKE_C_CLANG_TIDY 
            ${TIDY};
            --checks=readability-*,-readability-magic-numbers,performance-*,portability-*,-clang-analyzer-security.insecureAPI.*;
        )
    endif()
endif()

add_subdirectory("source/cppclient/client/")
option(IBKR_BUILD_TESTCPPCLIENT "Whether or not to build the TWS API example/test client" FALSE)
if( IBKR_BUILD_TESTCPPCLIENT )
    find_package( Threads )
    add_subdirectory("samples/Cpp/TestCppClient/")
endif()

set(CLANG_FORMAT_EXCLUDE_PATTERNS "build" "vcpkg")
file(GLOB_RECURSE ALL_SOURCE_FILES *.cpp *.h *.c)
foreach (SOURCE_FILE ${ALL_SOURCE_FILES})
    foreach (EXCLUDE_PATTERN ${CLANG_FORMAT_EXCLUDE_PATTERNS})
        string(FIND ${SOURCE_FILE} ${EXCLUDE_PATTERN} PROJECT_TRDPARTY_DIR_FOUND)
        if (NOT ${PROJECT_TRDPARTY_DIR_FOUND} EQUAL -1)
            list(REMOVE_ITEM ALL_SOURCE_FILES ${SOURCE_FILE})
        endif ()
    endforeach()
endforeach ()

find_program(FORMAT clang-format clang-format-10)
add_custom_target(
    format
    COMMAND ${FORMAT}
    -style=file
    -i
    ${ALL_SOURCE_FILES}
)

# install directories can be user defined
set(INSTALL_LIB_DIR lib CACHE PATH "Installation directory for libraries.")
set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for binaries.")
set(INSTALL_INCLUDE_DIR include CACHE PATH "Installation directory for header files.")
set(INSTALL_CMAKE_DIR ${DEF_INSTALL_CMAKE_DIR} CACHE PATH "Installation directory for CMake files.")

# Make relative paths absolute (needed later on)
foreach(p LIB BIN INCLUDE CMAKE)
  set(var INSTALL_${p}_DIR)
  if(NOT IS_ABSOLUTE "${${var}}")
    set(${var} "${CMAKE_INSTALL_PREFIX}/${${var}}")
  endif()
endforeach()

# set up include-directories
include_directories(
  "${PROJECT_SOURCE_DIR}"   # to find foo/foo.h
  "${PROJECT_BINARY_DIR}")  # to find foo/config.h

export(TARGETS twsapi FILE "${PROJECT_BINARY_DIR}/twsapiConfig.cmake")
export(PACKAGE twsapi)

# Create the twsapiConfig.cmake and twsapiConfigVersion files
file(RELATIVE_PATH REL_INCLUDE_DIR "${INSTALL_CMAKE_DIR}" "${INSTALL_INCLUDE_DIR}")
# ... for the build tree
set(CONF_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
configure_file(twsapiConfig.cmake.in "${PROJECT_BINARY_DIR}/twsapiConfig.cmake" @ONLY)
# ... for the install tree
set(CONF_INCLUDE_DIRS "\${twsapi_CMAKE_DIR}/${REL_INCLUDE_DIR}")
configure_file(twsapiConfig.cmake.in "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/twsapiConfig.cmake" @ONLY)
# ... for both
configure_file(twsapiConfigVersion.cmake.in "${PROJECT_BINARY_DIR}/twsapiConfigVersion.cmake" @ONLY)

# Install the twsapiConfig.cmake and twsapiConfigVersion.cmake
install(FILES
  "${PROJECT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/twsapiConfig.cmake"
  "${PROJECT_BINARY_DIR}/twsapiConfigVersion.cmake"
  DESTINATION "${INSTALL_CMAKE_DIR}" COMPONENT dev)

# Install the export set for use with the install-tree
install(EXPORT twsapi-targets DESTINATION
  "${INSTALL_CMAKE_DIR}" COMPONENT dev)
