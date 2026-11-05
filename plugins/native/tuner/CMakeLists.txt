# Vocal tuner (ADR-0011 N2): YIN pitch detection, PSOLA shifting, snapped to a
# key. The detector and the shifter are in include/sonora/native/tuner_pitch.h
# so the suite can drive each alone.
add_library(sonora_native_tuner STATIC src/tuner.cpp)
target_include_directories(sonora_native_tuner PUBLIC include)
target_link_libraries(sonora_native_tuner PUBLIC sonora_native_sdk sonora_dsp)

add_library(sonora_native_tuner_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_tuner_suite PUBLIC sonora_native_tuner sonora_dsp_measure)

sonora_native_quality_test(tuner)

# The LV2 and VST3 bundles, stereo and mono, through N1's format layer.
sonora_native_plugin(tuner FACTORY make_tuner DISPLAY "Sonora Tuner" UID SNtu MONO_UID SMtu
  LV2_CATEGORY "lv2:PitchPlugin" VST3_CATEGORIES "Fx|Pitch Shift")
