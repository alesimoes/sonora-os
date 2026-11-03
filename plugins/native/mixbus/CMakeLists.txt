# Mix bus (ADR-0011 N2 slice 7, part 1): the bus compressor and the width
# stage. The console colour is part 2.
add_library(sonora_native_mixbus STATIC src/mixbus.cpp)
target_include_directories(sonora_native_mixbus PUBLIC include)
target_link_libraries(sonora_native_mixbus PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_mixbus_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_mixbus_suite PUBLIC sonora_native_mixbus sonora_dsp_measure)

sonora_native_quality_test(mixbus)

# The LV2 and VST3 bundles, stereo and mono. No key input: a bus compressor
# keys from the bus.
sonora_native_plugin(mixbus FACTORY make_mixbus DISPLAY "Sonora Mix Bus" UID SNmb MONO_UID SMmb
  LV2_CATEGORY "lv2:CompressorPlugin" VST3_CATEGORIES "Fx|Dynamics")
