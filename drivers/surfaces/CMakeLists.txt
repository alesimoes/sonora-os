# Control surface protocols over MIDI ports (ADR-0055), on the boundary of
# ADR-0040. A surface is not a MIDI instrument: nothing here links the
# engine's MIDI model. The transport is a MidiPort; tests use a fake one.
add_library(sonora_surface_mcu STATIC
  src/mcu.cpp
  src/hui.cpp
  src/engine_bridge.cpp
  src/raw_midi_port.cpp
  src/mtc.cpp
  src/mtc_sync.cpp
)
target_include_directories(sonora_surface_mcu PUBLIC include)
target_link_libraries(sonora_surface_mcu PUBLIC sonora_surfaces sonora_api)

add_library(sonora_surface_testing INTERFACE)
target_include_directories(sonora_surface_testing INTERFACE testing)
target_link_libraries(sonora_surface_testing INTERFACE sonora_surface_mcu)

add_executable(test_mcu tests/test_mcu.cpp)
target_link_libraries(test_mcu PRIVATE sonora_surface_testing)
add_test(NAME surface_mcu COMMAND test_mcu)

add_executable(test_hui tests/test_hui.cpp)
target_link_libraries(test_hui PRIVATE sonora_surface_testing)
add_test(NAME surface_hui COMMAND test_hui)

add_executable(test_surface_bridge tests/test_engine_bridge.cpp)
target_link_libraries(test_surface_bridge PRIVATE sonora_surface_testing)
add_test(NAME surface_engine_bridge COMMAND test_surface_bridge)

add_executable(test_raw_midi_port tests/test_raw_midi_port.cpp)
target_link_libraries(test_raw_midi_port PRIVATE sonora_surface_testing)
add_test(NAME surface_raw_midi_port COMMAND test_raw_midi_port)

# MTC codec and chase (ADR-0122). Header-only; the root does not label
# drivers/surfaces yet, so the label is set here.
add_executable(test_mtc_contract tests/test_mtc_contract.cpp)
target_link_libraries(test_mtc_contract PRIVATE sonora_surface_testing)
add_test(NAME surface_mtc_contract COMMAND test_mtc_contract)
set_tests_properties(surface_mtc_contract PROPERTIES LABELS drivers)

# Chase and generation over the port (ADR-0122), against a real state block.
add_executable(test_mtc_sync tests/test_mtc_sync.cpp)
target_link_libraries(test_mtc_sync PRIVATE sonora_surface_testing)
add_test(NAME surface_mtc_sync COMMAND test_mtc_sync)
set_tests_properties(surface_mtc_sync PROPERTIES LABELS drivers)
