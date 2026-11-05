# Stereo imager: three-band width (ADR-0011 N0).
add_library(sonora_native_imager STATIC src/imager.cpp)
target_include_directories(sonora_native_imager PUBLIC include)
target_link_libraries(sonora_native_imager PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_imager_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_imager_suite PUBLIC sonora_native_imager sonora_dsp_measure)

sonora_native_quality_test(imager)

# The LV2 and VST3 bundles (ADR-0011 N1).
sonora_native_plugin(imager FACTORY make_imager DISPLAY "Sonora Imager" UID SNim MONO_UID SMim
  LV2_CATEGORY "lv2:SpatialPlugin" VST3_CATEGORIES "Fx|Spatial")
