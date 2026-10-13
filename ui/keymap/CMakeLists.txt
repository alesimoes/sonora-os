add_library(sonora_keymap STATIC src/keymap.cpp)
target_include_directories(sonora_keymap PUBLIC include)
target_link_libraries(sonora_keymap PUBLIC sonora_api sonora_base PRIVATE sonora_json)

add_executable(test_keymap tests/test_keymap.cpp)
# What the engine implements comes from FakeEngine's Hello (contracts/api
# testing), which engine/control's test holds to the real handlers. The
# handler tests went to engine/control and the terminal-mode tests to ui/tui
# in M4, so this test links nothing outside ui/keymap's own edges.
target_link_libraries(test_keymap PRIVATE sonora_keymap sonora_api_fake)
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
