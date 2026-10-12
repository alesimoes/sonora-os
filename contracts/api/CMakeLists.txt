# The command vocabulary. Header-only until M4 adds the wire codec.
add_library(sonora_api INTERFACE)
target_include_directories(sonora_api INTERFACE include)
target_link_libraries(sonora_api INTERFACE sonora_base)
