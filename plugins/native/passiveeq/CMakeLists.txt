# Passive EQ (ADR-0011 N2 analog family): the boost-and-cut network of a passive program equaliser.
add_library(sonora_native_passiveeq STATIC src/passiveeq.cpp)
target_include_directories(sonora_native_passiveeq PUBLIC include)
target_link_libraries(sonora_native_passiveeq PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_passiveeq_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_passiveeq_suite PUBLIC sonora_native_passiveeq sonora_dsp_measure)

sonora_native_quality_test(passiveeq)

sonora_native_plugin(passiveeq FACTORY make_passiveeq DISPLAY "Sonora Passive EQ" UID SNpq MONO_UID SMpq
  LV2_CATEGORY "lv2:EQPlugin" VST3_CATEGORIES "Fx|EQ")
