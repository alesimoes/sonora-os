# Transformer (ADR-0011 N2 slice 3): the iron of a line or output transformer.
add_library(sonora_native_transformer STATIC src/transformer.cpp)
target_include_directories(sonora_native_transformer PUBLIC include)
target_link_libraries(sonora_native_transformer PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_transformer_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_transformer_suite PUBLIC sonora_native_transformer sonora_dsp_measure)

sonora_native_quality_test(transformer)

sonora_native_plugin(transformer FACTORY make_transformer DISPLAY "Sonora Transformer" UID SNtf MONO_UID SMtf
  LV2_CATEGORY "lv2:DistortionPlugin" VST3_CATEGORIES "Fx|Distortion")
