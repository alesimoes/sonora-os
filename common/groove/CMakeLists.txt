# Groove templates (ADR-0115). Pure functions over common/model's MusicalMap,
# off the audio thread; a module of its own so the model's CMake is untouched.
add_library(sonora_groove STATIC src/groove.cpp)
target_include_directories(sonora_groove PUBLIC include)
target_link_libraries(sonora_groove PUBLIC sonora_model_audited)

add_executable(groove_test tests/test_groove.cpp)
target_link_libraries(groove_test PRIVATE sonora_groove)
add_test(NAME groove COMMAND groove_test)
