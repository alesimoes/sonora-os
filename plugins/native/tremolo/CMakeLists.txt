# Tremolo and auto-pan (ADR-0011 N0, modulation family).
add_library(sonora_native_tremolo STATIC src/tremolo.cpp)
target_include_directories(sonora_native_tremolo PUBLIC include)
target_link_libraries(sonora_native_tremolo PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_tremolo_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_tremolo_suite PUBLIC sonora_native_tremolo sonora_dsp_measure)

sonora_native_quality_test(tremolo)

# The LV2 and VST3 bundles (ADR-0011 N1).
sonora_native_plugin(tremolo FACTORY make_tremolo DISPLAY "Sonora Tremolo" UID SNtr MONO_UID SMtr
  LV2_CATEGORY "lv2:ModulatorPlugin" VST3_CATEGORIES "Fx|Modulation")
