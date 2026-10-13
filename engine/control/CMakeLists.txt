# The only module where engine/core and contracts/api meet. Also where the
# model meets the engine: the plan builder and its publisher (ADR-0007 §2,
# ADR-0009 §4).
add_library(sonora_control STATIC
  src/session_commands.cpp
  src/plan_builder.cpp
  src/playback_controller.cpp
  src/take_document.cpp
)
target_include_directories(sonora_control PUBLIC include)
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api sonora_model)

# Here rather than in engine/core because it also drives the session through
# the installed commands, and control may link core but not the reverse.
add_executable(test_takes tests/test_takes.cpp)
target_link_libraries(test_takes PRIVATE sonora_control)
add_test(NAME takes COMMAND test_takes)

add_executable(test_playback_control tests/test_playback_control.cpp)
target_link_libraries(test_playback_control PRIVATE sonora_control)
add_test(NAME playback_control COMMAND test_playback_control)

# Record then play back against drivers/fake (ADR-0007 §7): a test-only
# edge, as in engine/core; sonora_control links no driver.
add_executable(test_playback_fake tests/test_playback_fake.cpp)
target_link_libraries(test_playback_fake PRIVATE sonora_control sonora_backend_fake)
add_test(NAME playback_fake COMMAND test_playback_fake)
