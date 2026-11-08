# The key pass's slice of the build (ADR-0066 §5): sources and tests only.
# app/cmd_key.cpp replaces its stub by existing (CMakeLists.txt).
target_sources(sonora_analyze PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/key/classical_key_detector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/key/decimator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/key/key_profiles.cpp)
# The pass's own headers, reachable as "key/...", not part of the contract.
target_include_directories(sonora_analyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# cmd_key.cpp is compiled into the test so the exit codes (0, 1, 130) and the
# no-report-on-cancel rule run without a subprocess and a signal.
add_executable(test_analyze_key tests/test_key.cpp app/cmd_key.cpp)
target_include_directories(test_analyze_key PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(test_analyze_key PRIVATE sonora_analyze sonora_audiofile sonora_json sonora_dsp)
add_test(NAME analyze_key COMMAND test_analyze_key)
