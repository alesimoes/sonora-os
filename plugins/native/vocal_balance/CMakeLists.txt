# Vocal Balance (ADR-0011 N5): a dynamic resonance suppressor, and the
# whole-track analysis and plan that set it. The processor is a neutral
# module like any other; the analysis and the plan source run on a control
# thread and link the session model (the plan is made of model members), so
# they are a library of their own and the processor never links them.
add_library(sonora_native_vocal_balance STATIC src/vocal_balance.cpp)
target_include_directories(sonora_native_vocal_balance PUBLIC include)
target_link_libraries(sonora_native_vocal_balance PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_vocal_balance_analysis STATIC src/analysis.cpp src/plan_source.cpp)
target_include_directories(sonora_native_vocal_balance_analysis PUBLIC include)
target_link_libraries(sonora_native_vocal_balance_analysis PUBLIC sonora_model PRIVATE sonora_dsp)

add_library(sonora_native_vocal_balance_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_vocal_balance_suite PUBLIC sonora_native_vocal_balance sonora_dsp_measure)

sonora_native_quality_test(vocal_balance)

# Synthetic vocals against the analysis and the plan it proposes.
add_executable(test_vocal_balance_analysis tests/test_analysis.cpp)
target_link_libraries(test_vocal_balance_analysis PRIVATE
  sonora_native_vocal_balance_analysis sonora_native_vocal_balance sonora_dsp_measure sonora_dsp sonora_model_audited)
target_include_directories(test_vocal_balance_analysis PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
add_test(NAME native_vocal_balance_analysis COMMAND test_vocal_balance_analysis)

sonora_native_plugin(vocal_balance FACTORY make_vocal_balance DISPLAY "Sonora Vocal Balance" UID SNvb MONO_UID SMvb
  LV2_CATEGORY "lv2:EQPlugin" VST3_CATEGORIES "Fx|EQ")
