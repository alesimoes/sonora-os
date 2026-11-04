# De-esser, a split-band sibilance compressor (ADR-0011 N0).
add_library(sonora_native_deesser STATIC src/deesser.cpp)
target_include_directories(sonora_native_deesser PUBLIC include)
target_link_libraries(sonora_native_deesser PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_deesser_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_deesser_suite PUBLIC sonora_native_deesser sonora_dsp_measure)

sonora_native_quality_test(deesser)

# The LV2 and VST3 bundles (ADR-0011 N1). No key input: the detector is the
# processor's own high band.
sonora_native_plugin(deesser FACTORY make_deesser DISPLAY "Sonora De-esser" UID SNde MONO_UID SMde
  LV2_CATEGORY "lv2:DynamicsPlugin" VST3_CATEGORIES "Fx|Dynamics")
