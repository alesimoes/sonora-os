# Strict JSON (ADR-0009 §5). Layer 0 beside common/base, standard library
# only. Not part of common/base because a DOM allocates and common/base is
# RT-safe by definition. Any module may link it except common/base and
# contracts/*.
add_library(sonora_json STATIC src/json.cpp)
target_include_directories(sonora_json PUBLIC include)

add_executable(test_json tests/test_json.cpp)
target_link_libraries(test_json PRIVATE sonora_json)
add_test(NAME json COMMAND test_json)
