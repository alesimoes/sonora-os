# Vari-Mu (ADR-0011 N2, native.analog): a variable-mu tube compressor,
# held to the three-halves law of its remote-cutoff gain cell and to its
# switched time constants (tests/suite.cpp).
add_library(sonora_native_varimu STATIC src/varimu.cpp)
target_include_directories(sonora_native_varimu PUBLIC include)
target_link_libraries(sonora_native_varimu PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_varimu_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_varimu_suite PUBLIC sonora_native_varimu sonora_dsp_measure)

sonora_native_quality_test(varimu)
# Releases of up to 25 s (the slow network) and aliasing at three rates and
# qualities: fewer cases under SONORA_DSP_QUICK=1, all in the nightly run.
set_tests_properties(native_varimu PROPERTIES LABELS slow)

# The LV2 and VST3 bundles, stereo and mono, with the key input.
sonora_native_plugin(varimu FACTORY make_varimu DISPLAY "Sonora Vari-Mu" UID SNvm MONO_UID SMvm
  LV2_CATEGORY "lv2:CompressorPlugin" VST3_CATEGORIES "Fx|Dynamics" SIDECHAIN)
