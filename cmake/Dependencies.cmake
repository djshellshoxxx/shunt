# Third-party dependencies, pinned. Set SHUNT_DEPS_DIR to a folder holding
# pre-cloned copies (juce, juce7, link, civetweb, json, monocypher) to build offline.
include(FetchContent)
set(SHUNT_DEPS_DIR "" CACHE PATH "Folder with pre-cloned dependencies")

# shunt_dep(<var> <dir> <git url> <tag>): sets <var> to the source directory.
function(shunt_dep var dir url tag)
  if(SHUNT_DEPS_DIR AND EXISTS "${SHUNT_DEPS_DIR}/${dir}")
    set(${var} "${SHUNT_DEPS_DIR}/${dir}" PARENT_SCOPE)
    return()
  endif()
  FetchContent_Declare(${dir} GIT_REPOSITORY ${url} GIT_TAG ${tag} GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
  FetchContent_GetProperties(${dir})
  if(NOT ${dir}_POPULATED)
    FetchContent_Populate(${dir})
  endif()
  set(${var} "${${dir}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

shunt_dep(SHUNT_JSON_DIR json https://github.com/nlohmann/json.git v3.11.3)
shunt_dep(SHUNT_CIVETWEB_DIR civetweb https://github.com/civetweb/civetweb.git v1.16)
shunt_dep(SHUNT_MONOCYPHER_DIR monocypher https://github.com/LoupVaillant/Monocypher.git 4.0.2)
if(SHUNT_WITH_LINK)
  shunt_dep(SHUNT_LINK_DIR link https://github.com/Ableton/link.git Link-4.1)
endif()
if(SHUNT_BUILD_APPS)
  if(MINGW)
    shunt_dep(SHUNT_JUCE_DIR juce7 https://github.com/juce-framework/JUCE.git 7.0.12)
  else()
    shunt_dep(SHUNT_JUCE_DIR juce https://github.com/juce-framework/JUCE.git 8.0.15)
  endif()
endif()

# nlohmann/json (MIT), header only
add_library(shunt_json INTERFACE)
target_include_directories(shunt_json SYSTEM INTERFACE ${SHUNT_JSON_DIR}/single_include)

# civetweb (MIT): HTTP and WebSocket, compiled directly, no TLS
add_library(shunt_civetweb STATIC ${SHUNT_CIVETWEB_DIR}/src/civetweb.c)
target_include_directories(shunt_civetweb SYSTEM PUBLIC ${SHUNT_CIVETWEB_DIR}/include)
target_compile_definitions(shunt_civetweb PUBLIC NO_SSL USE_WEBSOCKET NO_CGI NO_CACHING MG_EXPERIMENTAL_INTERFACES=0 PRIVATE NO_FILESYSTEMS=0)
set_target_properties(shunt_civetweb PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(shunt_civetweb PRIVATE -w)
endif()
if(WIN32)
  target_link_libraries(shunt_civetweb PUBLIC ws2_32)
else()
  target_link_libraries(shunt_civetweb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Monocypher (BSD-2 / CC0): Ed25519 licence-key signatures
add_library(shunt_monocypher STATIC
  ${SHUNT_MONOCYPHER_DIR}/src/monocypher.c
  ${SHUNT_MONOCYPHER_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(shunt_monocypher SYSTEM PUBLIC ${SHUNT_MONOCYPHER_DIR}/src ${SHUNT_MONOCYPHER_DIR}/src/optional)
set_target_properties(shunt_monocypher PROPERTIES C_STANDARD 99)

# Ableton Link (GPLv2+ or proprietary licence; see LEGAL.md)
if(SHUNT_WITH_LINK)
  include(${SHUNT_LINK_DIR}/AbletonLinkConfig.cmake)
endif()
