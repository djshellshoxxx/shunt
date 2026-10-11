add_executable(shuntsim shuntsim/main.cpp)
target_link_libraries(shuntsim PRIVATE shunt_net)

add_executable(shunt_cli shunt_cli/main.cpp)
target_link_libraries(shunt_cli PRIVATE shunt_net shunt_clock shunt_log shunt_version)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shunt_keygen/main.cpp)
  add_executable(shunt_keygen shunt_keygen/main.cpp)
  target_link_libraries(shunt_keygen PRIVATE shunt_core)
endif()
