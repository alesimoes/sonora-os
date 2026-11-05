# Transient shaper: attack and sustain by differential envelope (ADR-0011 N0).
add_library(sonora_native_transient STATIC src/transient.cpp)
target_include_directories(sonora_native_transient PUBLIC include)
target_link_libraries(sonora_native_transient PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_transient_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_transient_suite PUBLIC sonora_native_transient sonora_dsp_measure)

sonora_native_quality_test(transient)

# The LV2 and VST3 bundles (ADR-0011 N1). MEMORYLESS: at its defaults
# (attack, sustain and gain 0 dB) the output is the input whatever the
# detectors hold, so the driver's "a burst left state" proof cannot see them.
sonora_native_plugin(transient FACTORY make_transient DISPLAY "Sonora Transient" UID SNts MONO_UID SMts
  LV2_CATEGORY "lv2:DynamicsPlugin" VST3_CATEGORIES "Fx|Dynamics" MEMORYLESS)
