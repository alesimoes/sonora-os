# Tape (ADR-0011 N2 slice 4): record and playback through magnetic tape,
# the Jiles-Atherton hysteresis of common/dsp tape.h as a processor.
add_library(sonora_native_tape STATIC src/tape.cpp)
target_include_directories(sonora_native_tape PUBLIC include)
target_link_libraries(sonora_native_tape PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_tape_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_tape_suite PUBLIC sonora_native_tape sonora_dsp_measure)

sonora_native_quality_test(tape)
# Aliasing at three rates and three qualities, the response at three rates:
# one rate and fewer tones under SONORA_DSP_QUICK=1 (PR CI, sanitizers), all in the nightly run.
set_tests_properties(native_tape PROPERTIES LABELS slow)

# The LV2 and VST3 bundles, stereo and mono, through N1's format layer.
sonora_native_plugin(tape FACTORY make_tape DISPLAY "Sonora Tape" UID SNtp MONO_UID SMtp
  EXPERIMENTAL LV2_CATEGORY "lv2:DistortionPlugin" VST3_CATEGORIES "Fx|Distortion")
