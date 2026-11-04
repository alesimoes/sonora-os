# sonora-analyze (ADR-0011 3.4, ADR-0046): offline analysis passes and the mix
# assistant's planner. Never on an audio thread; it allocates and reads files.
# Linux-only for now because it links common/model (the plan contract,
# ADR-0038), which is.
add_library(sonora_analyze STATIC
  src/mastering_plan.cpp
  src/mastering_rules.cpp
  src/mix_analysis.cpp)
target_include_directories(sonora_analyze PUBLIC include)
target_link_libraries(sonora_analyze PUBLIC sonora_model sonora_native_mastering PRIVATE sonora_dsp)

add_executable(sonora-analyze app/main.cpp)
target_link_libraries(sonora-analyze PRIVATE sonora_analyze sonora_audiofile sonora_json)

add_executable(test_analyze_mastering tests/test_mastering.cpp)
target_link_libraries(test_analyze_mastering PRIVATE sonora_analyze sonora_dsp)
add_test(NAME analyze_mastering COMMAND test_analyze_mastering)
