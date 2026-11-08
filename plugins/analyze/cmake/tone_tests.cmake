# Tests of the tone-match pass (ADR-0072), included from tone.cmake after
# tone_cmd.cmake, whose sonora-analyze and generator targets the command test runs.
set(_tone_deny "${CMAKE_CURRENT_SOURCE_DIR}/../native/ampcab/tools/ampcab_name_deny.txt")
get_filename_component(_tone_deny ${_tone_deny} ABSOLUTE)

add_executable(test_analyze_tone_matcher tests/test_tone_matcher.cpp)
target_link_libraries(test_analyze_tone_matcher PRIVATE sonora_analyze)
add_test(NAME analyze_tone_matcher COMMAND test_analyze_tone_matcher)
set_tests_properties(analyze_tone_matcher PROPERTIES LABELS analyze)

if(TARGET ampcab-tone-profile)
  add_executable(test_analyze_tone_command tests/test_tone_command.cpp)
  target_link_libraries(test_analyze_tone_command PRIVATE sonora_analyze_tone_measure
    sonora_native_ampcab_cards sonora_sha256 sonora_json)
  # The hygiene case skips with a message when the deny list is absent.
  if(EXISTS "${_tone_deny}")
    target_compile_definitions(test_analyze_tone_command PRIVATE AMPCAB_NAME_DENY_FILE="${_tone_deny}")
  endif()
  add_test(NAME analyze_tone_command
    COMMAND test_analyze_tone_command $<TARGET_FILE:sonora-analyze> $<TARGET_FILE:ampcab-tone-profile>
            ${_tone_ampcab}/tests/data/tiny.nam ${CMAKE_CURRENT_BINARY_DIR}/tone_command_fixture)
  set_tests_properties(analyze_tone_command PROPERTIES LABELS analyze)
endif()
