# The session document (ADR-0007 §1, M8; ADR-0009). Layer 1. Standard C++17
# only (ADR-0008) and never on the audio thread (ADR-0007 §2): it allocates,
# shares ownership and holds strings. engine/control, ui/client and
# interchange link it; engine/core sees only the plan built from it.
set(SONORA_MODEL_SOURCES
  src/apply.cpp
  src/history.cpp
  src/local_edit.cpp
  src/model.cpp
  src/op_codec.cpp
  src/replication.cpp
  src/session_file.cpp
)
add_library(sonora_model STATIC ${SONORA_MODEL_SOURCES})
target_include_directories(sonora_model PUBLIC include)
target_link_libraries(sonora_model PRIVATE sonora_json)
# Debug builds audit every playlist edit (src/local_edit.h): the range-local
# operators and check against the whole-playlist ones, aborting on any
# difference.
target_compile_definitions(sonora_model PRIVATE $<$<CONFIG:Debug>:SONORA_MODEL_AUDIT=1>)

# The same library, audited in every configuration, for the model's own
# tests: the Release build CI runs still checks the local verdict against
# playlist_problem() after every member of every compound.
add_library(sonora_model_audited STATIC ${SONORA_MODEL_SOURCES})
target_include_directories(sonora_model_audited PUBLIC include)
target_link_libraries(sonora_model_audited PRIVATE sonora_json)
target_compile_definitions(sonora_model_audited PRIVATE SONORA_MODEL_AUDIT=1)

add_executable(test_model_apply tests/test_apply.cpp)
target_link_libraries(test_model_apply PRIVATE sonora_model_audited)
add_test(NAME model_apply COMMAND test_model_apply)

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

# The replication format: the op codec and Document/Replica (ADR-0009 �2).
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

# What a compound costs on a long playlist; run by hand, numbers in ADR-0009.
add_executable(measure_compound tests/measure_compound.cpp)
target_link_libraries(measure_compound PRIVATE sonora_model)
