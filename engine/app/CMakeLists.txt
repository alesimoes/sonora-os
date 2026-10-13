add_executable(sonora-engine src/main.cpp src/quit_signals.cpp src/memory_lock.cpp)
target_include_directories(sonora-engine PRIVATE include)
# main.cpp is still the terminal interface (M4 turns it into a client of
# InProcessEngine), so until then it links ui/keymap and ui/tui, which
# ADR-0007 §1 forbids to anything outside ui/.
target_link_libraries(sonora-engine PRIVATE
  sonora_control sonora_core sonora_backend_jack sonora_keymap sonora_tui)

install(TARGETS sonora-engine RUNTIME DESTINATION bin)

# Compiled in again rather than shared through a library: the test needs one
# file, and an engine/app library is not a target ADR-0007 defines.
add_executable(test_quit_signals tests/test_quit_signals.cpp src/quit_signals.cpp)
target_include_directories(test_quit_signals PRIVATE include)
add_test(NAME quit_signals COMMAND test_quit_signals)

add_executable(test_memory_lock tests/test_memory_lock.cpp src/memory_lock.cpp)
target_include_directories(test_memory_lock PRIVATE include)
add_test(NAME memory_lock COMMAND test_memory_lock)
