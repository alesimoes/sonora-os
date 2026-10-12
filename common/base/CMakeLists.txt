# RT-safe primitives. Depends on nothing but the standard library.
add_library(sonora_base INTERFACE)
target_include_directories(sonora_base INTERFACE include)

add_executable(test_ringbuffer tests/test_ringbuffer.cpp)
target_link_libraries(test_ringbuffer PRIVATE sonora_base Threads::Threads)
add_test(NAME ringbuffer COMMAND test_ringbuffer)
