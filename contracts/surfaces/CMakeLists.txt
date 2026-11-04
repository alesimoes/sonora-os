# Control surface boundary (ADR-0007 4.4, ADR-0040): events in, feedback out,
# two lock-free rings. Header-only; no HUI/MCU code lives here.
add_library(sonora_surfaces INTERFACE)
target_include_directories(sonora_surfaces INTERFACE include)
target_link_libraries(sonora_surfaces INTERFACE sonora_base)

find_package(Threads REQUIRED)
add_executable(test_surface_link tests/test_surface_link.cpp)
target_link_libraries(test_surface_link PRIVATE sonora_surfaces Threads::Threads)
add_test(NAME surface_link COMMAND test_surface_link)
