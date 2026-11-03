# Phaser and flanger (ADR-0011 N0, modulation family).
add_library(sonora_native_phaser STATIC src/phaser.cpp)
target_include_directories(sonora_native_phaser PUBLIC include)
target_link_libraries(sonora_native_phaser PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_phaser_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_phaser_suite PUBLIC sonora_native_phaser sonora_dsp_measure)

sonora_native_quality_test(phaser)

# The LV2 and VST3 bundles (ADR-0011 N1).
sonora_native_plugin(phaser FACTORY make_phaser DISPLAY "Sonora Phaser" UID SNph MONO_UID SMph
  LV2_CATEGORY "lv2:PhaserPlugin" VST3_CATEGORIES "Fx|Modulation")
