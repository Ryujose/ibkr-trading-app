file(GLOB SOURCES "*.cpp")
add_executable(TestCppClient ${SOURCES})
set_target_properties(TestCppClient
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(TestCppClient PRIVATE twsapi)
target_link_libraries(TestCppClient PUBLIC Threads::Threads)
target_include_directories(TestCppClient PRIVATE "${PROJECT_SOURCE_DIR}/source/cppclient/" "${PROJECT_SOURCE_DIR}/source/cppclient/client/")

if( MSVC )
    target_compile_options( TestCppClient PRIVATE /W3 )
else()
    target_compile_options( TestCppClient PRIVATE -Wall -Wextra -pedantic -Wno-unused-parameter )
endif()

install(TARGETS TestCppClient DESTINATION bin)

FILE(GLOB HEADERS "*.h")
install(FILES ${HEADERS} DESTINATION "include/tws-api")