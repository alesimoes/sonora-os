add_executable(sonora-engine
  src/main.cpp src/quit_signals.cpp src/memory_lock.cpp src/exit_watchdog.cpp
  src/device_policy.cpp src/plugin_paths.cpp)
target_include_directories(sonora-engine PRIVATE include)
# Headless since M7: interfaces connect on the control socket. Nothing of
# ui/ is linked. On a terminal the engine starts sonora-tui, a separate
# executable, from beside itself; the dependency below only builds it there.
target_link_libraries(sonora-engine PRIVATE
  sonora_control sonora_core sonora_backend_jack sonora_backend_fake sonora_platform
  Threads::Threads)
add_dependencies(sonora-engine sonora-tui)

install(TARGETS sonora-engine RUNTIME DESTINATION bin)

# Compiled in again rather than shared through a library: the test needs one
# file, and an engine/app library is not a target ADR-0007 defines.
add_executable(test_quit_signals tests/test_quit_signals.cpp src/quit_signals.cpp)
target_include_directories(test_quit_signals PRIVATE include)
target_link_libraries(test_quit_signals PRIVATE Threads::Threads)
add_test(NAME quit_signals COMMAND test_quit_signals)

add_executable(test_memory_lock tests/test_memory_lock.cpp src/memory_lock.cpp)
target_include_directories(test_memory_lock PRIVATE include)
add_test(NAME memory_lock COMMAND test_memory_lock)

add_executable(test_exit_watchdog tests/test_exit_watchdog.cpp src/exit_watchdog.cpp)
target_include_directories(test_exit_watchdog PRIVATE include)
target_link_libraries(test_exit_watchdog PRIVATE Threads::Threads)
add_test(NAME exit_watchdog COMMAND test_exit_watchdog)

# The plugin catalog cache's default path and the image build id (ADR-0012,
# amendment of 2026-10-19, changes A and B).
add_executable(test_plugin_paths tests/test_plugin_paths.cpp src/plugin_paths.cpp)
target_include_directories(test_plugin_paths PRIVATE include)
add_test(NAME plugin_paths COMMAND test_plugin_paths)

add_executable(test_device_policy tests/test_device_policy.cpp src/device_policy.cpp)
target_include_directories(test_device_policy PRIVATE include)
target_link_libraries(test_device_policy PRIVATE sonora_backend_api)
add_test(NAME device_policy COMMAND test_device_policy)

# The engine end to end on drivers/fake (ADR-0007 M3): no audio server.
add_test(NAME engine_fake_record
  COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:sonora-engine>
          -DDIR=${CMAKE_CURRENT_BINARY_DIR}/fake_engine_session
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_engine_record.cmake)
set_tests_properties(engine_fake_record PROPERTIES
  SKIP_REGULAR_EXPRESSION "cannot lock memory")

# One session folder, one engine (the folder lock, common/platform).
add_test(NAME engine_two_engines_one_session
  COMMAND ${CMAKE_COMMAND} -DENGINE=$<TARGET_FILE:sonora-engine>
          -DDIR=${CMAKE_CURRENT_BINARY_DIR}/two_engines_session
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_engine_two_engines.cmake)
