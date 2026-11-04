# Saturator and exciter (ADR-0011 core stock set): an oversampled waveshaper.
add_library(sonora_native_saturator STATIC src/saturator.cpp)
target_include_directories(sonora_native_saturator PUBLIC include)
target_link_libraries(sonora_native_saturator PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_saturator_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_saturator_suite PUBLIC sonora_native_saturator sonora_dsp_measure)

sonora_native_quality_test(saturator)
# Aliasing at three rates, three qualities and three shapes: fewer of each
# under SONORA_DSP_QUICK=1 (PR CI, sanitizers), all in the nightly run.
set_tests_properties(native_saturator PROPERTIES LABELS slow)

# The LV2 and VST3 bundles, stereo and mono, through N1's format layer.
sonora_native_plugin(saturator FACTORY make_saturator DISPLAY "Sonora Saturator" UID SNsa MONO_UID SMsa
  LV2_CATEGORY "lv2:DistortionPlugin" VST3_CATEGORIES "Fx|Distortion")
