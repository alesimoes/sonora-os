# The session document (ADR-0007 §1, M8; ADR-0009). Layer 1. Standard C++17
# only (ADR-0008) and never on the audio thread (ADR-0007 §2): it allocates,
# shares ownership and holds strings. engine/control, ui/client and
# interchange link it; engine/core sees only the plan built from it.
add_library(sonora_model STATIC
  src/apply.cpp
  src/history.cpp
  src/model.cpp
  src/op_codec.cpp
  src/replication.cpp
  src/session_file.cpp
)
target_include_directories(sonora_model PUBLIC include)
target_link_libraries(sonora_model PRIVATE sonora_json)

add_executable(test_model_apply tests/test_apply.cpp)
target_link_libraries(test_model_apply PRIVATE sonora_model)
add_test(NAME model_apply COMMAND test_model_apply)

add_executable(test_model_history tests/test_history.cpp)
target_link_libraries(test_model_history PRIVATE sonora_model)
add_test(NAME model_history COMMAND test_model_history)

# The frozen fixtures, one per format version, must load forever
# (session-format.md, "Versioning").
add_executable(test_model_session_file tests/test_session_file.cpp)
target_link_libraries(test_model_session_file PRIVATE sonora_model)
target_compile_definitions(test_model_session_file PRIVATE
  SONORA_MODEL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME model_session_file COMMAND test_model_session_file)

add_executable(test_model_random_edits tests/test_random_edits.cpp)
target_link_libraries(test_model_random_edits PRIVATE sonora_model)
add_test(NAME model_random_edits COMMAND test_model_random_edits)

# The replication format: the op codec and Document/Replica (ADR-0009 �2).
add_executable(test_model_replication tests/test_replication.cpp)
target_link_libraries(test_model_replication PRIVATE sonora_model)
add_test(NAME model_replication COMMAND test_model_replication)
