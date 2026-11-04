cmake_minimum_required(VERSION 3.16)
project(Shunt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# FS-TEST section 1: -DSHUNT_SANITIZE=address (ASan+UBSan) or thread (TSan).
set(SHUNT_SANITIZE "" CACHE STRING "Sanitizer: '', address, thread")
if(SHUNT_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(SHUNT_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT SHUNT_SANITIZE STREQUAL "")
  message(FATAL_ERROR "SHUNT_SANITIZE must be '', address or thread")
endif()

find_package(Threads REQUIRED)

# ---- shunt_net: packets, device table, claim machine, master tracker, pcapng, sockets
add_library(shunt_net STATIC
  net/src/Packets.cpp
  net/src/DeviceTable.cpp
  net/src/ClaimMachine.cpp
  net/src/MasterTracker.cpp
  net/src/Pcapng.cpp
  net/src/NetworkStack.cpp
  net/src/PosixSocket.cpp
)
target_include_directories(shunt_net PUBLIC net/include)
target_link_libraries(shunt_net PUBLIC Threads::Threads)

# ---- shunt_clock: estimator, timeline, state machine (no sockets)
add_library(shunt_clock STATIC clock/src/ClockEngine.cpp)
target_include_directories(shunt_clock PUBLIC clock/include net/include)

# ---- shunt_log: tracklist events and exporters
add_library(shunt_log STATIC log/src/Tracklist.cpp log/src/RekordboxXml.cpp)
target_include_directories(shunt_log PUBLIC log/include net/include)

# ---- shunt_meta: dbserver client and streaming rekordbox XML reader (RS-04)
file(GLOB SHUNT_META_SOURCES CONFIGURE_DEPENDS meta/src/*.cpp)
add_library(shunt_meta STATIC ${SHUNT_META_SOURCES})
target_include_directories(shunt_meta PUBLIC meta/include)
target_link_libraries(shunt_meta PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(shunt_meta PUBLIC ws2_32)
endif()

# ---- shunt_out: OSC, MIDI clock, Link logic, output manager (no JUCE, no third-party code)
add_library(shunt_out STATIC
  out/src/Osc.cpp
  out/src/Midi.cpp
  out/src/Link.cpp
  out/src/OutputManager.cpp
)
target_include_directories(shunt_out PUBLIC out/include)
target_link_libraries(shunt_out PUBLIC shunt_clock shunt_net)
option(SHUNT_WITH_LINK "Build the Ableton Link SDK binding (needs Link.hpp on the include path)" OFF)
if(SHUNT_WITH_LINK)
  target_compile_definitions(shunt_out PUBLIC SHUNT_WITH_LINK=1)
endif()

# ---- shunt_app: JSON, settings, core runtime, HTTP/WebSocket server, embedded web UI
foreach(ui index overlay)
  string(SUBSTRING ${ui} 0 1 first)
  string(TOUPPER ${first} FIRST)
  string(SUBSTRING ${ui} 1 -1 rest)
  set(sym k${FIRST}${rest}Html)
  set(gen ${CMAKE_BINARY_DIR}/gen/${ui}_html.cpp)
  add_custom_command(OUTPUT ${gen}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/ui/${ui}.html -DOUT=${gen} -DSYMBOL=${sym} -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/ui/${ui}.html ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
  list(APPEND UI_SOURCES ${gen})
endforeach()
add_library(shunt_app STATIC
  app/src/Json.cpp
  app/src/Settings.cpp
  app/src/Http.cpp
  app/src/Core.cpp
  app/src/Api.cpp
  ${UI_SOURCES}
)
target_include_directories(shunt_app PUBLIC app/include)
target_link_libraries(shunt_app PUBLIC shunt_net shunt_clock shunt_log shunt_out shunt_meta Threads::Threads)

# ---- tools
add_executable(shunt_headless tools/shunt_headless/main.cpp)
target_link_libraries(shunt_headless PRIVATE shunt_app)

# Simulator: main plus the scenario registry (FS-TEST section 2).
file(GLOB SHUNT_SIM_SOURCES CONFIGURE_DEPENDS tools/shuntsim/*.cpp tools/shuntsim/scenarios/*.cpp)
add_executable(shuntsim ${SHUNT_SIM_SOURCES})
target_include_directories(shuntsim PRIVATE tools/shuntsim)
target_link_libraries(shuntsim PRIVATE shunt_net)

add_executable(shunt_cli tools/shunt_cli/main.cpp)
target_link_libraries(shunt_cli PRIVATE shunt_net shunt_clock shunt_log)

# ---- tests
enable_testing()
# Tests are globbed so features add files, not CMake edits (FS-00 section 2).
file(GLOB SHUNT_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp tests/features/test_*.cpp)
add_executable(ShuntTests tests/main.cpp ${SHUNT_TEST_SOURCES})
target_include_directories(ShuntTests PRIVATE tests)
target_link_libraries(ShuntTests PRIVATE shunt_net shunt_clock shunt_log shunt_out shunt_app)
add_test(NAME ShuntTests COMMAND ShuntTests)
add_test(NAME shuntsim_localhost
  COMMAND shunt_cli --addr 127.0.0.1 --mode follow --selftest-sim $<TARGET_FILE:shuntsim>)
set_tests_properties(shuntsim_localhost PROPERTIES TIMEOUT 60)
if(CMAKE_CROSSCOMPILING_EMULATOR)
  # The selftest spawns the cross-built simulator; prefix it with the emulator.
  list(JOIN CMAKE_CROSSCOMPILING_EMULATOR " " _shunt_emu)
  set_tests_properties(shuntsim_localhost PROPERTIES ENVIRONMENT "SHUNT_SIM_LAUNCHER=${_shunt_emu}")
endif()

# ---- N-T2 replay tests against real captures (fetched by tools/fetch_captures.sh)
set(SHUNT_CAPTURES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/captures CACHE PATH "Directory with reference captures")
function(shunt_replay_test name file regex)
  if(EXISTS ${SHUNT_CAPTURES_DIR}/${file})
    add_test(NAME replay_${name} COMMAND shunt_cli --replay ${SHUNT_CAPTURES_DIR}/${file} --speed 0)
    set_tests_properties(replay_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}"
                         FAIL_REGULAR_EXPRESSION "[1-9][0-9]* parse errors" TIMEOUT 120)
  endif()
endfunction()
# Golden expectations: device numbers/kinds and master behaviour, zero parse errors on 50000..50002.
shunt_replay_test(powerup powerup.pcapng "device 2 joined CDJ-2000nexus \\(nexus\\).*: [0-9]+ parsed, 0 parse errors, 3 devices, 0 master changes")
shunt_replay_test(to_virtual to-virtual.pcapng "device 5 joined Virtual CDJ.*: [0-9]+ parsed, 0 parse errors, 4 devices")
shunt_replay_test(s06_load_and_play S06-load-and-play.pcapng "master 0 -> 1.*0 parse errors, 2 devices, 1 master changes, 1[0-9][0-9] master beats, bpm 12[78]\\.")
shunt_replay_test(sync_master Sync-Master.pcapng "master 2 -> 33.*master 33 -> 3.*0 parse errors, 3 devices, 6 master changes")

# Metadata tests are a separate binary (fake dbserver on loopback).
file(GLOB SHUNT_META_TESTS CONFIGURE_DEPENDS tests/meta/*.cpp)
add_executable(ShuntMetaTests tests/main.cpp ${SHUNT_META_TESTS})
target_include_directories(ShuntMetaTests PRIVATE tests)
target_link_libraries(ShuntMetaTests PRIVATE shunt_meta)
add_test(NAME ShuntMetaTests COMMAND ShuntMetaTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
