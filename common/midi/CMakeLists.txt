# MIDI 1.0 in musical time: events, notes, tempo map, Standard MIDI Files,
# edit operations, live byte-stream parsing. Layer 1 beside common/audiofile,
# platform-neutral (ADR-0008), and never on the audio thread (ADR-0007 §2):
# the audio thread's MIDI type is sonora::MidiBuffer, in common/base.
add_library(sonora_midi STATIC
  src/event.cpp
  src/tempo_map.cpp
  src/notes.cpp
  src/smf.cpp
  src/edit.cpp
  src/stream_parser.cpp
)
target_include_directories(sonora_midi PUBLIC include)
target_link_libraries(sonora_midi PUBLIC sonora_base)

foreach(name midi_event midi_tempo_map midi_notes midi_smf midi_edit midi_stream_parser)
  add_executable(test_${name} tests/test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE sonora_midi)
  add_test(NAME ${name} COMMAND test_${name})
  set_tests_properties(${name} PROPERTIES LABELS midi)
endforeach()
