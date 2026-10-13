# Chorus (ADR-0011 N0).
add_library(sonora_native_chorus STATIC src/chorus.cpp)
target_include_directories(sonora_native_chorus PUBLIC include)
target_link_libraries(sonora_native_chorus PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_chorus_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_chorus_suite PUBLIC sonora_native_chorus sonora_dsp_measure)

sonora_native_quality_test(chorus)

# The LV2 and VST3 bundles (ADR-0011 N1).
sonora_native_plugin(chorus FACTORY make_chorus DISPLAY "Sonora Chorus" UID SNch
  LV2_CATEGORY "lv2:ChorusPlugin" VST3_CATEGORIES "Fx|Modulation")
