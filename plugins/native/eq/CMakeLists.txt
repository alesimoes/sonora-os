# Parametric EQ (ADR-0011 N0). The processor is neutral C++17 on common/dsp
# and the SDK; its suite runs in sonora-dsp-measure.
add_library(sonora_native_eq STATIC src/eq.cpp)
target_include_directories(sonora_native_eq PUBLIC include)
target_link_libraries(sonora_native_eq PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_eq_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_eq_suite PUBLIC sonora_native_eq sonora_dsp_measure)

sonora_native_quality_test(eq)

# The LV2 and VST3 bundles (ADR-0011 N1).
sonora_native_plugin(eq FACTORY make_eq DISPLAY "Sonora EQ" UID SNeq
  LV2_CATEGORY "lv2:EQPlugin" VST3_CATEGORIES "Fx|EQ")
