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

# ---- tools
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
)
target_include_directories(ShuntTests PRIVATE tests)
target_link_libraries(ShuntTests PRIVATE shunt_net shunt_clock shunt_log)
add_test(NAME ShuntTests COMMAND ShuntTests)
add_test(NAME shuntsim_localhost
  COMMAND shunt_cli --addr 127.0.0.1 --mode follow --selftest-sim $<TARGET_FILE:shuntsim>)
set_tests_properties(shuntsim_localhost PROPERTIES TIMEOUT 60)
