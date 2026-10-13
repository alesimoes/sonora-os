# The terminal interface, a client of the control API (ADR-0007 M4). A
# library until M7 makes it the sonora-tui executable; until then
# sonora-engine runs it over InProcessEngine. It links contracts/api and
# ui/keymap and nothing of engine/ (ADR-0007 §1).
add_library(sonora_tui STATIC src/tty_mode.cpp src/terminal_ui.cpp)
target_include_directories(sonora_tui PUBLIC include)
target_link_libraries(sonora_tui PUBLIC sonora_api sonora_keymap)

# The terminal mode, moved here from ui/keymap's test in M4.
add_executable(test_tty_mode tests/test_tty_mode.cpp)
target_link_libraries(test_tty_mode PRIVATE sonora_tui sonora_api_fake)
add_test(NAME tty_mode COMMAND test_tty_mode)

# The interface against FakeEngine, the provider's fake (ADR-0007 §7).
add_executable(test_terminal_ui tests/test_terminal_ui.cpp)
target_link_libraries(test_terminal_ui PRIVATE sonora_tui sonora_api_fake)
add_test(NAME terminal_ui COMMAND test_terminal_ui)
