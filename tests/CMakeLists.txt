# One test binary per module so modules can be developed and run independently.
function(shunt_test_binary name pattern)
  file(GLOB srcs CONFIGURE_DEPENDS ${pattern})
  if(NOT srcs)
    return()
  endif()
  add_executable(${name} main.cpp ${srcs})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

shunt_test_binary(ShuntTests "test_*.cpp" shunt_net shunt_clock shunt_log)
shunt_test_binary(ShuntOutTests "out/*.cpp" shunt_out)
shunt_test_binary(ShuntMetaTests "meta/*.cpp" shunt_meta)
shunt_test_binary(ShuntCoreTests "core/*.cpp" shunt_core)

if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME shuntsim_localhost
    COMMAND shunt_cli --addr 127.0.0.1 --mode follow --selftest-sim $<TARGET_FILE:shuntsim>)
  set_tests_properties(shuntsim_localhost PROPERTIES TIMEOUT 60)
endif()
