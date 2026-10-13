# DRAFT (ADR-0007 §4.3, Phase 4): the shared-memory layout between the engine
# and a plugin host process. Proposed by plugin-engineer for rt-engineer
# review; nothing in engine/ uses it yet. Header-only and platform-neutral
# (ADR-0008): layout, offsets and the parameter ring, no OS calls.
add_library(sonora_plugin_protocol INTERFACE)
target_include_directories(sonora_plugin_protocol INTERFACE include)
target_link_libraries(sonora_plugin_protocol INTERFACE sonora_base)

add_executable(test_plugin_protocol tests/test_protocol.cpp)
target_link_libraries(test_plugin_protocol PRIVATE sonora_plugin_protocol)
add_test(NAME plugin_protocol COMMAND test_plugin_protocol)
