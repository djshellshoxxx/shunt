file(GLOB SHUNT_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(shunt_core STATIC ${SHUNT_CORE_SOURCES})
target_include_directories(shunt_core PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(shunt_core
  PUBLIC shunt_net shunt_clock shunt_log shunt_out shunt_meta shunt_version
  PRIVATE shunt_json shunt_civetweb shunt_monocypher)
if(SHUNT_REQUIRE_LICENCE)
  target_compile_definitions(shunt_core PRIVATE SHUNT_REQUIRE_LICENCE=1)
endif()
# Web pages (core/web/*) are embedded as C string literals at build time.
file(GLOB SHUNT_WEB_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/*)
set(SHUNT_WEB_GEN ${CMAKE_CURRENT_BINARY_DIR}/WebAssets.cpp)
add_custom_command(OUTPUT ${SHUNT_WEB_GEN}
  COMMAND ${CMAKE_COMMAND} -DOUT=${SHUNT_WEB_GEN} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/web
          -P ${CMAKE_CURRENT_SOURCE_DIR}/EmbedWeb.cmake
  DEPENDS ${SHUNT_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/EmbedWeb.cmake)
target_sources(shunt_core PRIVATE ${SHUNT_WEB_GEN})
