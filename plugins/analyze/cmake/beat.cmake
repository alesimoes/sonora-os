# The beat pass's slice of the build (ADR-0066 §5, ADR-0114): sources and tests
# only. app/cmd_beat.cpp replaces its stub by existing (CMakeLists.txt). The
# decimator is the key pass's (key.cmake adds its source and the src/ include).
target_sources(sonora_analyze PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/beat/classical_beat_detector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/beat/onset_envelope.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/beat/tempo_track.cpp)
target_include_directories(sonora_analyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# cmd_beat.cpp is compiled into the test so the exit codes (0, 1, 130) and the
# no-report-on-cancel rule run without a subprocess and a signal.
add_executable(test_analyze_beat tests/test_beat.cpp app/cmd_beat.cpp)
target_include_directories(test_analyze_beat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(test_analyze_beat PRIVATE sonora_analyze sonora_audiofile sonora_json sonora_dsp)
add_test(NAME analyze_beat COMMAND test_analyze_beat)
set_tests_properties(analyze_beat PROPERTIES LABELS analyze)
