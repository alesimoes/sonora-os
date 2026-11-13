# Interchange (ADR-0007 section 4.5, M10): other formats to and from the
# native session, offline, in the short-lived sonora-convert process. Layer 3
# on common/model, common/midi and common/session_files. Nothing here is
# reachable from the audio thread, and engine/core never links it.
add_library(sonora_interchange STATIC
  src/aaf_import.cpp
  src/aaf_map.cpp
  src/aaf_rational.cpp
  src/convert.cpp
  src/session_io.cpp
  src/smf_export.cpp
  src/smf_import.cpp
)
target_include_directories(sonora_interchange PUBLIC include)
target_link_libraries(sonora_interchange PUBLIC sonora_model sonora_midi sonora_session_files)

add_executable(sonora-convert app/main.cpp)
target_link_libraries(sonora-convert PRIVATE sonora_interchange)

# Golden sessions in and out, an SMF round trip, and the command line.
add_executable(test_interchange tests/test_interchange.cpp)
target_link_libraries(test_interchange PRIVATE sonora_interchange)
target_compile_definitions(test_interchange PRIVATE
  SONORA_MODEL_FIXTURES="${CMAKE_SOURCE_DIR}/common/model/tests/fixtures")
add_test(NAME interchange COMMAND test_interchange)

# AAF contract: the neutral document, the reader seam and the importer's errors.
add_executable(test_aaf_contract tests/test_aaf_contract.cpp)
target_link_libraries(test_aaf_contract PRIVATE sonora_interchange)
target_include_directories(test_aaf_contract PRIVATE tests)
add_test(NAME aaf_contract COMMAND test_aaf_contract)

# AAF mapper on hand-built documents: 25 fps, transition, filler, missing UMID,
# overlap refused.
add_executable(test_aaf_map tests/test_aaf_map.cpp)
target_link_libraries(test_aaf_map PRIVATE sonora_interchange)
target_include_directories(test_aaf_map PRIVATE src)
add_test(NAME aaf_map COMMAND test_aaf_map)
