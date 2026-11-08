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

# Offline evaluation on real voices (ADR-0077); nothing in it links the
# processor's audio path.
add_subdirectory(eval)

# The scorer: library, CLI and test (ADR-0077). WAV reading is common/audiofile's.
if(TARGET sonora_audiofile)
  add_library(sonora_native_tuner_eval_lib STATIC eval/src/eval_signal.cpp eval/src/eval_dataset.cpp)
  target_include_directories(sonora_native_tuner_eval_lib PUBLIC eval/src)
  target_link_libraries(sonora_native_tuner_eval_lib PUBLIC sonora_native_tuner_eval sonora_native_tuner PRIVATE sonora_audiofile)
  add_executable(native_tuner_eval eval/app/main.cpp)
  target_link_libraries(native_tuner_eval PRIVATE sonora_native_tuner_eval_lib)
  add_executable(native_tuner_eval_test eval/tests/test_eval_tool.cpp)
  target_link_libraries(native_tuner_eval_test PRIVATE sonora_native_tuner_eval_lib)
  add_test(NAME native_tuner_eval_test COMMAND native_tuner_eval_test)
endif()
