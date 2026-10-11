cmake_minimum_required(VERSION 3.22)
project(Shunt VERSION 0.9.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SHUNT_BUILD_APPS "Build the JUCE desktop app and shunt_headless" ON)
option(SHUNT_WITH_LINK "Build the Ableton Link output (GPLv2+ unless a proprietary Link licence is held)" ON)
option(SHUNT_BUILD_TESTS "Build unit tests" ON)
option(SHUNT_REQUIRE_LICENCE "Release builds: run as Shunt Monitor (no outputs) without a valid key file" OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
endif()
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(MINGW)
  include_directories(BEFORE SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/cmake/mingw-shim)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

include(cmake/Dependencies.cmake)

# Version header shared by every target
configure_file(cmake/Version.h.in ${CMAKE_BINARY_DIR}/generated/shunt/Version.h @ONLY)
add_library(shunt_version INTERFACE)
target_include_directories(shunt_version INTERFACE ${CMAKE_BINARY_DIR}/generated)

# ---- core libraries (plain C++17, no JUCE)
add_subdirectory(net)      # shunt_net: packets, devices, claim, master, pcapng, sockets
add_subdirectory(clock)    # shunt_clock: estimator, timeline, state machine
add_subdirectory(log)      # shunt_log: tracklist events, exporters, session files
add_subdirectory(out)      # shunt_out: Link, MIDI clock, OSC (the "clockout" library)
add_subdirectory(meta)     # shunt_meta: dbserver client, rekordbox XML
add_subdirectory(core)     # shunt_core: engine, settings, web server, licence

# ---- tools
add_subdirectory(tools)

# ---- applications (JUCE)
if(SHUNT_BUILD_APPS)
  add_subdirectory(${SHUNT_JUCE_DIR} juce EXCLUDE_FROM_ALL)
  add_subdirectory(app)
endif()

# ---- tests
if(SHUNT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
