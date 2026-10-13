# True-peak limiter (ADR-0011 N0).
add_library(sonora_native_limiter STATIC src/limiter.cpp)
target_include_directories(sonora_native_limiter PUBLIC include)
target_link_libraries(sonora_native_limiter PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_limiter_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_limiter_suite PUBLIC sonora_native_limiter sonora_dsp_measure)

sonora_native_quality_test(limiter)
