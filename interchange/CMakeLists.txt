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
target_link_libraries(sonora_interchange PUBLIC sonora_model sonora_midi sonora_session_files sonora_audiofile)

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

# libaaf (LGPL-2.1+) reads the AAF container. Optional and never fetched: with
# the option off, sonora-convert says so and exits 1 instead of importing
# nothing. The library is found, not built, so the offline build stays offline.
option(SONORA_WITH_LIBAAF "Read AAF files through libaaf" OFF)
if(SONORA_WITH_LIBAAF)
  find_path(LIBAAF_INCLUDE_DIR libaaf.h)
  find_library(LIBAAF_LIBRARY aaf)
  if(NOT LIBAAF_INCLUDE_DIR OR NOT LIBAAF_LIBRARY)
    message(FATAL_ERROR "SONORA_WITH_LIBAAF is ON but libaaf was not found")
  endif()
  target_sources(sonora_interchange PRIVATE src/aaf_libaaf_reader.cpp)
  target_include_directories(sonora_interchange PRIVATE ${LIBAAF_INCLUDE_DIR})
  target_link_libraries(sonora_interchange PUBLIC ${LIBAAF_LIBRARY})

  add_executable(test_aaf_libaaf tests/test_aaf_libaaf.cpp)
  target_link_libraries(test_aaf_libaaf PRIVATE sonora_interchange)
  target_compile_definitions(test_aaf_libaaf PRIVATE
    SONORA_AAF_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME aaf_libaaf COMMAND test_aaf_libaaf)
else()
  target_sources(sonora_interchange PRIVATE src/aaf_reader_off.cpp)
endif()

# OMF import (ADR-0160), read-only.
add_subdirectory(omf)
add_subdirectory(aaf_export)
add_subdirectory(ptx)

# sonora-convert dispatches .omf: run_convert needs the OMF importer, and
# sonora_omf already links sonora_interchange (a static cycle is legal).
target_link_libraries(sonora_interchange PUBLIC sonora_omf)
# ptx glue (convert_ptx.cpp) needs the mapper and media copy.
target_sources(sonora_interchange PRIVATE src/convert_ptx.cpp)
target_link_libraries(sonora_interchange PUBLIC sonora_ptx_map sonora_ptx_media)
if(TARGET sonora_ptx_libptformat)
  target_compile_definitions(sonora_interchange PRIVATE SONORA_WITH_LIBPTFORMAT=1)
  target_link_libraries(sonora_interchange PUBLIC sonora_ptx_libptformat)
endif()

add_executable(test_omf_convert tests/test_omf_convert.cpp)
target_link_libraries(test_omf_convert PRIVATE sonora_interchange)
add_test(NAME omf_convert COMMAND test_omf_convert)
set_tests_properties(omf_convert PROPERTIES LABELS interchange)

if(NOT TARGET sonora_ptx_libptformat)
  target_compile_definitions(test_interchange PRIVATE SONORA_PTX_READER_OFF=1)
endif()
