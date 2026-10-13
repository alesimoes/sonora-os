# The only target that may include <jack/jack.h>. It does so in src/ only, and
# links JACK privately, so nothing that links this compiles against JACK.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_library(sonora_backend_jack STATIC src/jack_backend.cpp)
target_include_directories(sonora_backend_jack PUBLIC include PRIVATE src)
target_link_libraries(sonora_backend_jack
  PUBLIC sonora_backend_api sonora_base
  PRIVATE PkgConfig::JACK)

add_executable(test_port_order tests/test_port_order.cpp)
target_link_libraries(test_port_order PRIVATE sonora_backend_jack)
add_test(NAME port_order COMMAND test_port_order)

# PeriodGuard needs no server, so its test does not link JACK either.
add_executable(test_period_guard tests/test_period_guard.cpp)
target_include_directories(test_period_guard PRIVATE src)
target_link_libraries(test_period_guard PRIVATE sonora_base Threads::Threads)
add_test(NAME period_guard COMMAND test_period_guard)
