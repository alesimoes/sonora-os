# tempo_from_beats (ADR-0114): the beat grid to a tempo map. Pure; links
# sonora_model through sonora_analyze.
target_sources(sonora_analyze PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/beat_tempo/beat_tempo.cpp)

add_executable(test_analyze_beat_tempo tests/test_beat_tempo.cpp)
target_link_libraries(test_analyze_beat_tempo PRIVATE sonora_analyze)
add_test(NAME analyze_beat_tempo COMMAND test_analyze_beat_tempo)
set_tests_properties(analyze_beat_tempo PROPERTIES LABELS analyze)
