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

# HostControl, wire version 1 (ADR-0012 §4): the engine <-> host socket's
# messages, frames, the OS-free Link, the state blob and its file name. No
# OS calls (ADR-0008): the socket pair is common/platform's ControlChannel.
add_library(sonora_plugin_host_control STATIC src/host_control.cpp
            include/sonora/plugin/host_control.h)
target_link_libraries(sonora_plugin_host_control PUBLIC sonora_plugin_protocol)

add_executable(test_plugin_host_control tests/test_host_control.cpp)
target_link_libraries(test_plugin_host_control PRIVATE sonora_plugin_host_control)
add_test(NAME plugin_host_control COMMAND test_plugin_host_control)

# The probe record, record version 1 (ADR-0012, amendment of 2026-10-19,
# item 2): what `sonora-plugin-host --probe` prints and the engine's plugin
# catalog caches, with its one codec. Standard library only, like the rest
# of contracts/ (ADR-0007): no common/json here.
add_library(sonora_plugin_probe_record STATIC src/probe_record.cpp
            include/sonora/plugin/probe_record.h)
target_include_directories(sonora_plugin_probe_record PUBLIC include)
target_link_libraries(sonora_plugin_probe_record PUBLIC sonora_base)

add_executable(test_plugin_probe_record tests/test_probe_record.cpp)
target_link_libraries(test_plugin_probe_record PRIVATE sonora_plugin_probe_record)
add_test(NAME plugin_probe_record COMMAND test_plugin_probe_record)
