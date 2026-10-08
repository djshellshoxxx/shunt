cmake_minimum_required(VERSION 3.16)
project(Shunt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

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
add_library(shunt_log STATIC log/src/Tracklist.cpp)
target_include_directories(shunt_log PUBLIC log/include net/include)

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
target_link_libraries(shunt_app PUBLIC shunt_net shunt_clock shunt_log shunt_out Threads::Threads)

# ---- tools
add_executable(shunt_headless tools/shunt_headless/main.cpp)
target_link_libraries(shunt_headless PRIVATE shunt_app)

add_executable(shuntsim tools/shuntsim/main.cpp)
target_link_libraries(shuntsim PRIVATE shunt_net)

add_executable(shunt_cli tools/shunt_cli/main.cpp)
target_link_libraries(shunt_cli PRIVATE shunt_net shunt_clock shunt_log)

# ---- tests
enable_testing()
add_executable(ShuntTests
  tests/main.cpp
  tests/test_packets.cpp
  tests/test_net.cpp
  tests/test_clock.cpp
  tests/test_log.cpp
  tests/test_out.cpp
  tests/test_app.cpp
)
target_include_directories(ShuntTests PRIVATE tests)
target_link_libraries(ShuntTests PRIVATE shunt_net shunt_clock shunt_log shunt_out shunt_app)
add_test(NAME ShuntTests COMMAND ShuntTests)
add_test(NAME shuntsim_localhost
  COMMAND shunt_cli --addr 127.0.0.1 --mode follow --selftest-sim $<TARGET_FILE:shuntsim>)
set_tests_properties(shuntsim_localhost PROPERTIES TIMEOUT 60)
