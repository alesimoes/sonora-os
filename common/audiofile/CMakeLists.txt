add_library(sonora_audiofile STATIC src/wav_writer.cpp)
target_include_directories(sonora_audiofile PUBLIC include)
target_link_libraries(sonora_audiofile PUBLIC sonora_base)

add_executable(test_wav_writer tests/test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE sonora_audiofile)
add_test(NAME wav_writer COMMAND test_wav_writer)
