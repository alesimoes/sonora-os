# Utility: gain, polarity, swap, balance, width, mono sum, DC blocker (ADR-0011 N0).
add_library(sonora_native_utility STATIC src/utility.cpp)
target_include_directories(sonora_native_utility PUBLIC include)
target_link_libraries(sonora_native_utility PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_utility_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_utility_suite PUBLIC sonora_native_utility sonora_dsp_measure)

sonora_native_quality_test(utility)

# The LV2 and VST3 bundles (ADR-0011 N1). MEMORYLESS: at its defaults (DC
# blocker off) the processor is a unit matrix with no signal memory.
sonora_native_plugin(utility FACTORY make_utility DISPLAY "Sonora Utility" UID SNut MONO_UID SMut
  LV2_CATEGORY "lv2:UtilityPlugin" VST3_CATEGORIES "Fx|Tools" MEMORYLESS)
