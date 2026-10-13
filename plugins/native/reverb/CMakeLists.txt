# Reverb (ADR-0011 N2): the FDN core of common/dsp as a processor.
add_library(sonora_native_reverb STATIC src/reverb.cpp)
target_include_directories(sonora_native_reverb PUBLIC include)
target_link_libraries(sonora_native_reverb PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_reverb_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_reverb_suite PUBLIC sonora_native_reverb sonora_dsp_measure)

sonora_native_quality_test(reverb)
# The decay measurements run at 48 kHz only under SONORA_DSP_QUICK=1 (PR CI,
# sanitizers: 30 s under ASan), at all three rates in the nightly run.
set_tests_properties(native_reverb PROPERTIES LABELS slow)

# The LV2 and VST3 bundles, stereo and mono, through N1's format layer.
sonora_native_plugin(reverb FACTORY make_reverb DISPLAY "Sonora Reverb" UID SNrv MONO_UID SMrv
  LV2_CATEGORY "lv2:ReverbPlugin" VST3_CATEGORIES "Fx|Reverb")
