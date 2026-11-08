# The timbre pass (ADR-0066 §4): included by plugins/analyze/CMakeLists.txt.
target_sources(sonora_analyze PRIVATE
  src/timbre/timbre_measure.cpp
  src/timbre/timbre_fit.cpp
  src/timbre/timbre_match.cpp)
target_include_directories(sonora_analyze PUBLIC src/timbre)

add_executable(test_analyze_timbre tests/test_timbre.cpp)
target_link_libraries(test_analyze_timbre PRIVATE sonora_analyze sonora_dsp)
add_test(NAME analyze_timbre COMMAND test_analyze_timbre)

sonora_analyze_cli_test(analyze_cli_timbre_usage 2 "usage: sonora-analyze timbre" timbre)
sonora_analyze_cli_test(analyze_cli_timbre_missing 1 "no-such-reference.wav" timbre no-such-reference.wav no-such-target.wav)
