# The terminal interface. A library until M7 makes it the sonora-tui
# executable; until M4 its loop is still engine/app's main.cpp.
add_library(sonora_tui STATIC src/tty_mode.cpp)
target_include_directories(sonora_tui PUBLIC include)
