# The terminal interface, a client of the control API (ADR-0007 M4). It
# links contracts/api and ui/keymap and nothing of engine/ (ADR-0007 §1).
# Since M7 it is its own executable, sonora-tui, on ui/client's socket;
# sonora-engine starts it on a terminal but does not link it.
add_library(sonora_tui STATIC src/tty_mode.cpp src/terminal_ui.cpp)
target_include_directories(sonora_tui PUBLIC include)
target_link_libraries(sonora_tui PUBLIC sonora_api sonora_keymap)

add_executable(sonora-tui src/main.cpp)
target_link_libraries(sonora-tui PRIVATE sonora_tui sonora_client)
install(TARGETS sonora-tui RUNTIME DESTINATION bin)

# The terminal mode, moved here from ui/keymap's test in M4.
add_executable(test_tty_mode tests/test_tty_mode.cpp)
target_link_libraries(test_tty_mode PRIVATE sonora_tui sonora_api_fake)
add_test(NAME tty_mode COMMAND test_tty_mode)

# The interface against FakeEngine, the provider's fake (ADR-0007 §7).
add_executable(test_terminal_ui tests/test_terminal_ui.cpp)
target_link_libraries(test_terminal_ui PRIVATE sonora_tui sonora_api_fake)
add_test(NAME terminal_ui COMMAND test_terminal_ui)
