# The vocal-balance command (ADR-0104): included by plugins/analyze/CMakeLists.txt.
# app/cmd_vocal.cpp replaces its stub by existing. The analysis library links
# the session model, so it is Linux-only, like the rest of this module.
if(SONORA_PLATFORM STREQUAL "linux")
  target_link_libraries(sonora-analyze PRIVATE sonora_native_vocal_balance_analysis sonora_json)

  # cmd_vocal.cpp is compiled into the test so exit codes and the report run
  # without a subprocess.
  add_executable(analyze_vocal_command tests/test_vocal_command.cpp app/cmd_vocal.cpp)
  target_include_directories(analyze_vocal_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(analyze_vocal_command PRIVATE
    sonora_native_vocal_balance_analysis sonora_audiofile sonora_json sonora_model)
  add_test(NAME analyze_vocal_command COMMAND analyze_vocal_command)
  set_tests_properties(analyze_vocal_command PROPERTIES LABELS analyze)
endif()
