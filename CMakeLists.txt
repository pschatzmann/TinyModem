cmake_minimum_required(VERSION 3.18)

project(TinyModem LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TINYMODEM_BUILD_TOOLS "Build the tinymodem command line tool" ON)
option(TINYMODEM_BUILD_TESTS "Build the automatic tests" ON)
option(TINYMODEM_PORTAUDIO "Support audio devices via PortAudio (if found)" ON)
set(AUDIO_TOOLS_DIR "" CACHE PATH "Source directory of arduino-audio-tools")

# header only library
add_library(tinymodem-lib INTERFACE)
target_include_directories(tinymodem-lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT (TINYMODEM_BUILD_TOOLS OR TINYMODEM_BUILD_TESTS))
    return()
endif()

# arduino-audio-tools: use a sibling checkout (Arduino libraries folder) or
# download it. We only need the headers.
if(NOT AUDIO_TOOLS_DIR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../arduino-audio-tools/src/AudioTools.h)
    set(AUDIO_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../arduino-audio-tools)
endif()
if(NOT AUDIO_TOOLS_DIR)
    include(FetchContent)
    FetchContent_Declare(arduino_audio_tools
        GIT_REPOSITORY https://github.com/pschatzmann/arduino-audio-tools.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR do-not-add-subdirectory)
    FetchContent_MakeAvailable(arduino_audio_tools)
    set(AUDIO_TOOLS_DIR ${arduino_audio_tools_SOURCE_DIR})
endif()
message(STATUS "arduino-audio-tools: ${AUDIO_TOOLS_DIR}")

# desktop build of the library with the minimal Arduino emulation of the
# AudioTools: we provide our own main()
add_library(tinymodem-desktop INTERFACE)
find_package(Threads REQUIRED)
target_link_libraries(tinymodem-desktop INTERFACE tinymodem-lib Threads::Threads)
target_include_directories(tinymodem-desktop INTERFACE ${AUDIO_TOOLS_DIR}/src)
target_compile_definitions(tinymodem-desktop INTERFACE IS_MIN_DESKTOP NO_MAIN)
target_compile_options(tinymodem-desktop INTERFACE -Wno-format -Wno-unused-variable)

if(TINYMODEM_BUILD_TOOLS)
    add_subdirectory(tools/tinymodem)
endif()

if(TINYMODEM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
