# sonora-dsp-measure links every processor's suite: a wiring executable at
# the top of the native tree, as engine/app is for the engine.
add_executable(sonora-dsp-measure main.cpp host_fixture.cpp)
# Every processor with a tests/suite.cpp (SONORA_NATIVE_SUITES, from
# plugins/native/CMakeLists.txt), which also generates the dispatch and
# fixture tables main.cpp and host_fixture.cpp include.
set(_suites)
foreach(p ${SONORA_NATIVE_SUITES})
  list(APPEND _suites sonora_native_${p}_suite)
endforeach()
target_link_libraries(sonora-dsp-measure PRIVATE ${_suites})
target_include_directories(sonora-dsp-measure PRIVATE ${SONORA_NATIVE_GENERATED_DIR})
