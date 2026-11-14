if(NOT TARGET sonora_ui_edit_tools)
  add_subdirectory(${CMAKE_SOURCE_DIR}/ui/edit_tools ${CMAKE_BINARY_DIR}/ui/edit_tools)
endif()

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

# Documented vendor shortcuts against the profiles (ui.keymaps.coverage).
add_executable(test_keymap_coverage tests/test_keymap_coverage.cpp)
target_link_libraries(test_keymap_coverage PRIVATE sonora_keymap)
target_compile_definitions(test_keymap_coverage PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME keymap_coverage COMMAND test_keymap_coverage)

# ADR-0070: console chords from the console keymap. The implementation and its
# test arrive in later tasks; the guards mean they never edit this file.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/console_keys.cpp")
  add_library(sonora_keymap_console_keys STATIC src/console_keys.cpp)
  target_include_directories(sonora_keymap_console_keys PUBLIC include)
  target_link_libraries(sonora_keymap_console_keys PUBLIC sonora_keymap)
endif()

add_executable(test_console_keys_contract tests/test_console_keys_contract.cpp)
target_link_libraries(test_console_keys_contract PRIVATE sonora_keymap)
add_test(NAME keymap_console_contract COMMAND test_console_keys_contract)
set_property(TEST keymap_console_contract APPEND PROPERTY LABELS keymap)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_console_keys.cpp")
  add_executable(test_console_keys tests/test_console_keys.cpp)
  target_link_libraries(test_console_keys PRIVATE sonora_keymap_console_keys)
  add_test(NAME keymap_console_keys COMMAND test_console_keys)
  set_property(TEST keymap_console_keys APPEND PROPERTY LABELS keymap)
endif()
