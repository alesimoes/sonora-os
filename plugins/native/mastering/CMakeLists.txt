# Mastering console (ADR-0011 N4, ADR-0046): EQ, compressor, saturation,
# width, gain, true-peak limiter and a loudness-aware output stage, built
# from the other native processors.
add_library(sonora_native_mastering STATIC src/mastering.cpp)
target_include_directories(sonora_native_mastering PUBLIC include)
target_link_libraries(sonora_native_mastering PUBLIC sonora_native_sdk
  PRIVATE sonora_dsp sonora_native_eq sonora_native_compressor sonora_native_saturator sonora_native_limiter)

add_library(sonora_native_mastering_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_mastering_suite PUBLIC sonora_native_mastering sonora_dsp_measure
  sonora_native_eq sonora_native_compressor sonora_native_saturator sonora_native_limiter)

sonora_native_quality_test(mastering)
# Long renders (auto-level settles over tens of seconds): fewer in
# SONORA_DSP_QUICK=1, all in the nightly run.
set_tests_properties(native_mastering PROPERTIES LABELS slow)

sonora_native_plugin(mastering FACTORY make_mastering DISPLAY "Sonora Mastering Console" UID SNms MONO_UID SMms
  LV2_CATEGORY "lv2:DynamicsPlugin" VST3_CATEGORIES "Fx|Dynamics|Mastering")
