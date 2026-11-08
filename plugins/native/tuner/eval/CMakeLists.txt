# The tuner's evaluation on real voices (ADR-0077): offline scoring, never on
# the audio path. This slice is the contract (header-only) and its test; the
# dataset tool comes next and adds its sources here.
add_library(sonora_native_tuner_eval INTERFACE)
target_include_directories(sonora_native_tuner_eval INTERFACE include)

add_executable(test_native_tuner_eval tests/test_eval_contract.cpp)
target_link_libraries(test_native_tuner_eval PRIVATE sonora_native_tuner_eval)
add_test(NAME native_tuner_eval_contract COMMAND test_native_tuner_eval)
