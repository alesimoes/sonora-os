add_library(sonora_keymap STATIC src/keymap.cpp)
target_include_directories(sonora_keymap PUBLIC include)
target_link_libraries(sonora_keymap PUBLIC sonora_api sonora_base PRIVATE sonora_json)

add_executable(test_keymap tests/test_keymap.cpp)
# Until M4 this test drives the handlers sonora-engine really installs, on a
# real Session, and checks bytes against the terminal mode main.cpp sets. So
# it links engine/control and ui/tui, an edge no ui/ module may have. M4 moves
# the handlers behind EngineControl and this link goes with it.
target_link_libraries(test_keymap PRIVATE sonora_keymap sonora_control sonora_tui)
# The shipped profiles are part of what this test checks, so it needs to find
# them wherever the build tree happens to be.
target_compile_definitions(test_keymap PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME keymap COMMAND test_keymap)

# Its own process, because setlocale is process-wide (BACKLOG: keymap strtod
# under de_DE).
add_executable(test_keymap_locale tests/test_keymap_locale.cpp)
target_link_libraries(test_keymap_locale PRIVATE sonora_keymap)
add_test(NAME keymap_locale COMMAND test_keymap_locale)
