# The tone command and the profile generator (ADR-0072), included from
# tone.cmake. app/cmd_tone.cpp replaces the stub by existing (CMakeLists.txt);
# the generator lives here because the amp-and-cab track's tooling is the one
# that renders captures, but it is built with the module that owns the
# profile schema.
get_filename_component(_tone_ampcab ${CMAKE_CURRENT_SOURCE_DIR}/../native/ampcab ABSOLUTE)
get_filename_component(_tone_repo ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(_tone_defs
  SONORA_TONE_DEFAULT_CAPTURES="${_tone_ampcab}/captures"
  SONORA_TONE_DEFAULT_CARDS="${_tone_repo}/brain/models")

# The fingerprint of an audio source and the stimulus: one definition for
# the reference and the captures.
add_library(sonora_analyze_tone_measure STATIC tools/tone_measure.cpp)
target_include_directories(sonora_analyze_tone_measure PUBLIC tools)
target_link_libraries(sonora_analyze_tone_measure PUBLIC sonora_analyze)

target_link_libraries(sonora-analyze PRIVATE sonora_analyze_tone_measure
  sonora_native_ampcab_cards sonora_sha256)
target_compile_definitions(sonora-analyze PRIVATE ${_tone_defs})

add_executable(ampcab-tone-profile tools/tone_profile_gen.cpp)
target_link_libraries(ampcab-tone-profile PRIVATE sonora_analyze_tone_measure
  sonora_native_ampcab_nam sonora_native_ampcab_cards sonora_sha256 sonora_json)
target_compile_definitions(ampcab-tone-profile PRIVATE ${_tone_defs})

add_executable(test_analyze_tone_cmd tests/test_tone_cmd.cpp)
target_link_libraries(test_analyze_tone_cmd PRIVATE sonora_analyze_tone_measure
  sonora_native_ampcab_cards sonora_sha256 sonora_json)
add_test(NAME analyze_tone_cmd
  COMMAND test_analyze_tone_cmd $<TARGET_FILE:sonora-analyze> $<TARGET_FILE:ampcab-tone-profile>
          ${_tone_ampcab}/tests/data/tiny.nam ${CMAKE_CURRENT_BINARY_DIR}/tone_cmd_fixture)
set_tests_properties(analyze_tone_cmd PROPERTIES LABELS analyze)
