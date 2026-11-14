# The pitch note-list contract (ADR-0143): sonora/api/pitch.h is header-only
# and lives in contracts/api's include directory. Added from the time.pitch
# hook in plugins/analyze/CMakeLists.txt until contracts/api/CMakeLists.txt
# is free to take it (ADR-0143 §6); sonora_api is linked by name, it is
# defined later in the configure.
add_executable(test_pitch_param test_pitch_param.cpp)
target_link_libraries(test_pitch_param PRIVATE sonora_api)
add_test(NAME api_pitch_param COMMAND test_pitch_param)
# The root labels this directory as plugins/analyze's child; the contract
# label is the one that belongs to it.
set_tests_properties(api_pitch_param PROPERTIES LABELS contracts)
