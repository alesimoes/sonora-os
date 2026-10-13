# Delay (ADR-0011 N0).
add_library(sonora_native_delay STATIC src/delay.cpp)
target_include_directories(sonora_native_delay PUBLIC include)
target_link_libraries(sonora_native_delay PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_delay_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_delay_suite PUBLIC sonora_native_delay sonora_dsp_measure)

sonora_native_quality_test(delay)
