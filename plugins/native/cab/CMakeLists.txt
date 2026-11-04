# Cabinet (ADR-0011 stage N3, section 4.3): a response convolved by
# common/dsp's zero-latency partitioned convolver, loaded off the audio
# thread. The LV2 and VST3 bundles wait on the response being carried in
# plugin state (ADR-0029 section 6): see cab.h.
add_library(sonora_native_cab STATIC src/cab.cpp)
target_include_directories(sonora_native_cab PUBLIC include)
target_link_libraries(sonora_native_cab PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_cab_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_cab_suite PUBLIC sonora_native_cab sonora_dsp_measure Threads::Threads)

sonora_native_quality_test(cab)

# The WAV reader is common/audiofile's, which the neutral (Portable) build
# does not have; so is the test that reads a file.
if(TARGET sonora_audiofile)
  add_library(sonora_native_cab_file STATIC src/cab_file.cpp)
  target_link_libraries(sonora_native_cab_file PUBLIC sonora_native_cab PRIVATE sonora_audiofile)
  add_executable(test_native_cab_file tests/test_cab_file.cpp)
  target_link_libraries(test_native_cab_file PRIVATE sonora_native_cab_file sonora_dsp_measure)
  add_test(NAME native_cab_file COMMAND test_native_cab_file)
endif()
