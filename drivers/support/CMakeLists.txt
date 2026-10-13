# Helpers every backend shares: channel order (natural_less) and the period
# guard that makes a mid-session change fatal. Header only, owned by the
# driver squad. Not in common/base, whose rule is that everything in it is
# RT-safe: natural_less allocates. Linked by drivers/jack and, from M3,
# drivers/fake; nothing else needs it.
add_library(sonora_driver_support INTERFACE)
target_include_directories(sonora_driver_support INTERFACE include)
target_link_libraries(sonora_driver_support INTERFACE sonora_backend_api sonora_base)

add_executable(test_port_order tests/test_port_order.cpp)
target_link_libraries(test_port_order PRIVATE sonora_driver_support)
add_test(NAME port_order COMMAND test_port_order)

add_executable(test_period_guard tests/test_period_guard.cpp)
target_link_libraries(test_period_guard PRIVATE sonora_driver_support Threads::Threads)
add_test(NAME period_guard COMMAND test_period_guard)

add_executable(test_frame_clock tests/test_frame_clock.cpp)
target_link_libraries(test_frame_clock PRIVATE sonora_driver_support)
add_test(NAME frame_clock COMMAND test_frame_clock)

add_executable(test_port_devices tests/test_port_devices.cpp)
target_link_libraries(test_port_devices PRIVATE sonora_driver_support)
add_test(NAME port_devices COMMAND test_port_devices)

add_executable(test_source_watch tests/test_source_watch.cpp)
target_link_libraries(test_source_watch PRIVATE sonora_driver_support)
add_test(NAME source_watch COMMAND test_source_watch)
