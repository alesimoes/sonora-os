# The control API (ADR-0007 §4.1): the command vocabulary, the messages and
# their wire codec, the shared state block, StateView and EngineControl.
# Depends on common/base only (ADR-0007 §1).
add_library(sonora_api STATIC
  src/wire.cpp
  src/state_block.cpp
  src/state_view.cpp
)
target_include_directories(sonora_api PUBLIC include)
target_link_libraries(sonora_api PUBLIC sonora_base Threads::Threads)

# FakeEngine, the provider's fake (ADR-0007 §7), and the conformance suite it
# shares with InProcessEngine. Test support, not the contract: the fake links
# common/model so that its model path is the real apply(), not a second one.
add_subdirectory(testing)
add_subdirectory(conformance)

add_executable(test_api_wire tests/test_wire.cpp)
target_link_libraries(test_api_wire PRIVATE sonora_api)
add_test(NAME api_wire COMMAND test_api_wire)

# Bounce Mix's wire argument (contract only, protocol version pending).
add_executable(test_bounce_wire tests/test_bounce_wire.cpp)
target_link_libraries(test_bounce_wire PRIVATE sonora_api)
add_test(NAME bounce_wire COMMAND test_bounce_wire)

# Export Stems' wire argument over the bounce one (ADR-0085, no protocol bump).
add_executable(test_stems_wire tests/test_stems_wire.cpp)
target_link_libraries(test_stems_wire PRIVATE sonora_api)
add_test(NAME stems_wire COMMAND test_stems_wire)

# Render in place's wire argument (ADR-0051, protocol 56).
add_executable(test_render_wire tests/test_render_wire.cpp)
target_link_libraries(test_render_wire PRIVATE sonora_api)
add_test(NAME render_wire COMMAND test_render_wire)

# The blocked-by-operator marker on PluginInfo::refusal (ADR-0065).
add_executable(test_plugin_refusal tests/test_plugin_refusal.cpp)
target_link_libraries(test_plugin_refusal PRIVATE sonora_api)
add_test(NAME api_plugin_refusal COMMAND test_plugin_refusal)

add_executable(test_api_state_view tests/test_state_view.cpp)
target_link_libraries(test_api_state_view PRIVATE sonora_api)
add_test(NAME api_state_view COMMAND test_api_state_view)

add_executable(test_api_fake_engine tests/test_fake_engine.cpp)
target_link_libraries(test_api_fake_engine PRIVATE sonora_api_fake sonora_api_conformance
                                                   sonora_model)
add_test(NAME api_conformance_fake COMMAND test_api_fake_engine)
