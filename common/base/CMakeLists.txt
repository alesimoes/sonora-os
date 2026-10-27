# RT-safe primitives. Depends on nothing but the standard library.
add_library(sonora_base INTERFACE)
target_include_directories(sonora_base INTERFACE include)

add_executable(test_ringbuffer tests/test_ringbuffer.cpp)
target_link_libraries(test_ringbuffer PRIVATE sonora_base Threads::Threads)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_midi_buffer tests/test_midi_buffer.cpp)
target_link_libraries(test_midi_buffer PRIVATE sonora_base)
add_test(NAME midi_buffer COMMAND test_midi_buffer)

add_executable(test_fade_curve tests/test_fade_curve.cpp)
target_link_libraries(test_fade_curve PRIVATE sonora_base)
add_test(NAME fade_curve COMMAND test_fade_curve)

# SHA-256, which names the files beside a session (plugin state, MIDI
# sequences; ADR-0015 §3). Compiled, unlike the RT primitives above, and
# never on the audio thread.
add_library(sonora_sha256 STATIC src/sha256.cpp)
target_include_directories(sonora_sha256 PUBLIC include)

add_executable(test_sha256 tests/test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE sonora_sha256)
add_test(NAME sha256 COMMAND test_sha256)

# ADR-0015 Unverified 6: MiB/s on 100 MiB; run by hand, numbers in the PR.
add_executable(measure_sha256 tests/measure_sha256.cpp)
target_link_libraries(measure_sha256 PRIVATE sonora_sha256)
