# The session document (ADR-0007 §1, M8; ADR-0009). Layer 1. Standard C++17
# only (ADR-0008) and never on the audio thread (ADR-0007 §2): it allocates,
# shares ownership and holds strings. engine/control, ui/client and
# interchange link it; engine/core sees only the plan built from it. It links
# common/midi for notes (ADR-0014, the model-to-midi edge).
set(SONORA_MODEL_SOURCES
  src/apply.cpp
  src/history.cpp
  src/local_edit.cpp
  src/midi_json.cpp
  src/model.cpp
  src/musical_map.cpp
  src/op_codec.cpp
  src/sequence_file.cpp
  src/replication.cpp
  src/session_file.cpp
)
add_library(sonora_model STATIC ${SONORA_MODEL_SOURCES})
target_include_directories(sonora_model PUBLIC include)
target_link_libraries(sonora_model PUBLIC sonora_midi PRIVATE sonora_json sonora_sha256)
# Debug builds audit every playlist edit (src/local_edit.h): the range-local
# operators and check against the whole-playlist ones, aborting on any
# difference.
target_compile_definitions(sonora_model PRIVATE $<$<CONFIG:Debug>:SONORA_MODEL_AUDIT=1>)

# The same library, audited in every configuration, for the model's own
# tests: the Release build CI runs still checks the local verdict against
# playlist_problem() after every member of every compound.
add_library(sonora_model_audited STATIC ${SONORA_MODEL_SOURCES})
target_include_directories(sonora_model_audited PUBLIC include)
target_link_libraries(sonora_model_audited PUBLIC sonora_midi PRIVATE sonora_json sonora_sha256)
target_compile_definitions(sonora_model_audited PRIVATE SONORA_MODEL_AUDIT=1)

add_executable(test_model_apply tests/test_apply.cpp)
target_link_libraries(test_model_apply PRIVATE sonora_model_audited)
add_test(NAME model_apply COMMAND test_model_apply)

# ADR-0016, section 3: a loop take's passes on playlists of their own,
# the chosen one on the active playlist, one undo step, and on the wire.
add_executable(test_model_record_loop_take tests/test_record_loop_take.cpp)
target_link_libraries(test_model_record_loop_take PRIVATE sonora_model_audited)
add_test(NAME model_record_loop_take COMMAND test_model_record_loop_take)

add_executable(test_model_history tests/test_history.cpp)
target_link_libraries(test_model_history PRIVATE sonora_model_audited)
add_test(NAME model_history COMMAND test_model_history)

# The frozen fixtures, one per format version, must load forever
# (session-format.md, "Versioning").
add_executable(test_model_session_file tests/test_session_file.cpp)
target_link_libraries(test_model_session_file PRIVATE sonora_model_audited)
target_compile_definitions(test_model_session_file PRIVATE
  SONORA_MODEL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME model_session_file COMMAND test_model_session_file)

# Plugin inserts (ADR-0012 6): operations, undo under O1, refusals.
add_executable(test_model_inserts tests/test_inserts.cpp)
target_link_libraries(test_model_inserts PRIVATE sonora_model_audited)
add_test(NAME model_inserts COMMAND test_model_inserts)

add_executable(test_model_random_edits tests/test_random_edits.cpp)
target_link_libraries(test_model_random_edits PRIVATE sonora_model_audited)
add_test(NAME model_random_edits COMMAND test_model_random_edits)

# The replication format: the op codec and Document/Replica (ADR-0009 §2).
add_executable(test_model_replication tests/test_replication.cpp)
target_link_libraries(test_model_replication PRIVATE sonora_model_audited)
add_test(NAME model_replication COMMAND test_model_replication)

# Compound operations (ADR-0009 update 2026-10-14).
add_executable(test_model_compound tests/test_compound.cpp)
target_link_libraries(test_model_compound PRIVATE sonora_model_audited)
add_test(NAME model_compound COMMAND test_model_compound)

# Range-local validation (BACKLOG Track 2 item 8), audited member by member.
add_executable(test_model_local_edit tests/test_local_edit.cpp)
target_link_libraries(test_model_local_edit PRIVATE sonora_model_audited)
add_test(NAME model_local_edit COMMAND test_model_local_edit)

# MusicalMap (ADR-0014 §2, brain/specs/midi.md §2): exact conversions, bounds,
# negative rounding, the batch API, bars|beats.
add_executable(test_model_musical_map tests/test_musical_map.cpp)
target_link_libraries(test_model_musical_map PRIVATE sonora_model_audited)
add_test(NAME model_musical_map COMMAND test_model_musical_map)

# What MusicalMap costs against the control thread's 10 ms; run by hand,
# numbers in the M-1 PR.
add_executable(measure_musical_map tests/measure_musical_map.cpp)
target_link_libraries(measure_musical_map PRIVATE sonora_model)

# What a compound costs on a long playlist; run by hand, numbers in ADR-0009.
add_executable(measure_compound tests/measure_compound.cpp)
target_link_libraries(measure_compound PRIVATE sonora_model)

# MIDI in the model (ADR-0014 M-1): track kinds, MIDI regions as windows onto
# shared sequences, the timebase tag, the MIDI operations, the undo window.
add_executable(test_model_midi tests/test_midi.cpp)
target_link_libraries(test_model_midi PRIVATE sonora_model_audited)
add_test(NAME model_midi COMMAND test_model_midi)

# ADR-0015 §1, §2, §4, §5: canonical chunks, undo and the budget per chunk,
# the window's chunk table in parts, a Load in parts.
add_executable(test_model_midi_chunks tests/test_midi_chunks.cpp)
target_link_libraries(test_model_midi_chunks PRIVATE sonora_model_audited)
add_test(NAME model_midi_chunks COMMAND test_model_midi_chunks)

# ADR-0014 Unverified 6 and ADR-0015 Unverified 2: dense MIDI against the
# file caps and the undo window, and open and save at the 2^24-entry
# target; run by hand, numbers in the PRs.
add_executable(measure_midi_session tests/measure_midi_session.cpp)
target_link_libraries(measure_midi_session PRIVATE sonora_model sonora_json sonora_sha256)

# MIDI sequences beside the session file (ADR-0015 §3): the .msq bytes, the
# decoder's refusals and a fuzz sweep, digests, and which files may go (O4).
add_executable(test_model_sequence_file tests/test_sequence_file.cpp)
target_link_libraries(test_model_sequence_file PRIVATE sonora_model_audited)
add_test(NAME model_sequence_file COMMAND test_model_sequence_file)
