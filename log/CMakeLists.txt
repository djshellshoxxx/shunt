file(GLOB SHUNT_LOG_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(shunt_log STATIC ${SHUNT_LOG_SOURCES})
target_include_directories(shunt_log PUBLIC include)
target_link_libraries(shunt_log PUBLIC shunt_net)
