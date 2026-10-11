file(GLOB SHUNT_CLOCK_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(shunt_clock STATIC ${SHUNT_CLOCK_SOURCES})
target_include_directories(shunt_clock PUBLIC include)
target_link_libraries(shunt_clock PUBLIC shunt_net)
