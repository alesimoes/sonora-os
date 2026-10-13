add_library(sonora_audiofile STATIC src/wav_writer.cpp)
target_include_directories(sonora_audiofile PUBLIC include)
# common/platform for the take file's exclusive, close-on-exec create
# (ADR-0008 §3): the one OS-specific thing a writer needs.
target_link_libraries(sonora_audiofile PUBLIC sonora_base PRIVATE sonora_platform)

add_executable(test_wav_writer tests/test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE sonora_audiofile)
add_test(NAME wav_writer COMMAND test_wav_writer)

# A child process started while a take is open does not inherit it
# (ADR-0008 V3: OS-specific tests live in tests/posix, built if(UNIX)).
if(UNIX)
  add_executable(test_wav_writer_cloexec tests/posix/test_wav_writer_cloexec.cpp)
  target_link_libraries(test_wav_writer_cloexec PRIVATE sonora_audiofile sonora_platform)
  add_test(NAME wav_writer_cloexec COMMAND test_wav_writer_cloexec)
endif()
