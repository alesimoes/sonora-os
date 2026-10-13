# ACCEPTED, layout version 1 (ADR-0012 §5): the shared-memory layout between
# the engine and a plugin host process, and the session's read-only clock
# region. Owned jointly by plugin-engineer and rt-engineer (ADR-0007 §4.3).
# Header-only and platform-neutral (ADR-0008): layout, offsets, the
# parameter ring and the last-value table, no OS calls. The wake lives in
# common/platform (wake.h).
add_library(sonora_plugin_protocol INTERFACE)
target_include_directories(sonora_plugin_protocol INTERFACE include)
target_link_libraries(sonora_plugin_protocol INTERFACE sonora_base)

# The layout asserts are compile-time: this test failing to build is the
# contract test failing.
find_package(Threads REQUIRED)
add_executable(test_plugin_protocol tests/test_protocol.cpp)
target_link_libraries(test_plugin_protocol PRIVATE sonora_plugin_protocol Threads::Threads)
add_test(NAME plugin_protocol COMMAND test_plugin_protocol)
