add_library(sonora_audiofile STATIC src/wav_writer.cpp src/wav_reader.cpp src/wav_recovery.cpp)
target_include_directories(sonora_audiofile PUBLIC include)
# common/platform for the take file's exclusive, close-on-exec create and its
# periodic sync (ADR-0008 §3; ADR-0009 Consequences).
target_link_libraries(sonora_audiofile PUBLIC sonora_base PRIVATE sonora_platform)

add_executable(test_wav_writer tests/test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE sonora_audiofile)
add_test(NAME wav_writer COMMAND test_wav_writer)

add_executable(test_wav_reader tests/test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE sonora_audiofile)
add_test(NAME wav_reader COMMAND test_wav_reader)

# A child process started while a take is open does not inherit it
# (ADR-0008 V3: OS-specific tests live in tests/posix, built if(UNIX)).
if(UNIX)
  add_executable(test_wav_writer_cloexec tests/posix/test_wav_writer_cloexec.cpp)
  target_link_libraries(test_wav_writer_cloexec PRIVATE sonora_audiofile sonora_platform)
  add_test(NAME wav_writer_cloexec COMMAND test_wav_writer_cloexec)
endif()

# A take whose engine died before closing it gets the length on disk
# (session-format.md, "Crash recovery"); a finished or foreign file is left
# as it is.
add_executable(test_wav_recovery tests/test_wav_recovery.cpp)
target_link_libraries(test_wav_recovery PRIVATE sonora_audiofile)
add_test(NAME wav_recovery COMMAND test_wav_recovery)
