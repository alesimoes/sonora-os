# Compressor (ADR-0011 N0).
add_library(sonora_native_compressor STATIC src/compressor.cpp)
target_include_directories(sonora_native_compressor PUBLIC include)
target_link_libraries(sonora_native_compressor PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_compressor_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_compressor_suite PUBLIC sonora_native_compressor sonora_dsp_measure)

sonora_native_quality_test(compressor)

# The LV2 and VST3 bundles (ADR-0011 N1). No side-chain input yet: the
# host does not route one (brain/research/native-format-n1.md).
sonora_native_plugin(compressor FACTORY make_compressor DISPLAY "Sonora Compressor" UID SNcp MONO_UID SMcp
  LV2_CATEGORY "lv2:CompressorPlugin" VST3_CATEGORIES "Fx|Dynamics" SIDECHAIN)
