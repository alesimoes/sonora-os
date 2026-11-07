# Multiband compressor (plugins.stock): 3 or 4 bands, minimum-phase
# Linkwitz-Riley or linear-phase FIR crossovers (common/dsp/crossover.h).
add_library(sonora_native_multiband STATIC src/multiband.cpp)
target_include_directories(sonora_native_multiband PUBLIC include)
target_link_libraries(sonora_native_multiband PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_multiband_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_multiband_suite PUBLIC sonora_native_multiband sonora_dsp_measure)

sonora_native_quality_test(multiband)

# The LV2 and VST3 bundles (ADR-0011 N1). The band count, phase mode,
# crossovers and look-ahead reconfigure (they set the latency).
sonora_native_plugin(multiband FACTORY make_multiband DISPLAY "Sonora Multiband" UID SNmu MONO_UID SMmu
  LV2_CATEGORY "lv2:CompressorPlugin" VST3_CATEGORIES "Fx|Dynamics")
