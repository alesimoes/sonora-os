# The only target that may include <jack/jack.h>. Until M2 that header is in
# jack_backend.h, so JACK is linked PUBLIC and reaches whatever links this.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED jack)

add_library(sonora_backend_jack STATIC src/jack_backend.cpp)
target_include_directories(sonora_backend_jack PUBLIC include ${JACK_INCLUDE_DIRS})
target_link_directories(sonora_backend_jack PUBLIC ${JACK_LIBRARY_DIRS})
# sonora_core is the edge ADR-0007 names as temporarily illegal: process_cb
# calls Session::process until M2 puts contracts/backend between them.
target_link_libraries(sonora_backend_jack PUBLIC ${JACK_LIBRARIES} sonora_base sonora_core)

add_executable(test_port_order tests/test_port_order.cpp)
target_link_libraries(test_port_order PRIVATE sonora_backend_jack)
add_test(NAME port_order COMMAND test_port_order)
