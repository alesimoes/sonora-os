# The only module where engine/core and contracts/api meet.
add_library(sonora_control STATIC src/session_commands.cpp)
target_include_directories(sonora_control PUBLIC include)
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api)

# Here rather than in engine/core because it also drives the session through
# the installed commands, and control may link core but not the reverse.
add_executable(test_takes tests/test_takes.cpp)
target_link_libraries(test_takes PRIVATE sonora_control)
add_test(NAME takes COMMAND test_takes)
