cmake_minimum_required(VERSION 3.16)
project(sonora-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED jack)
find_package(Threads REQUIRED)

add_library(sonora_core STATIC
  src/wav_writer.cpp
  src/track.cpp
  src/disk_thread.cpp
  src/session.cpp
  src/jack_backend.cpp
)
target_include_directories(sonora_core PUBLIC include ${JACK_INCLUDE_DIRS})
target_link_libraries(sonora_core PUBLIC ${JACK_LIBRARIES} Threads::Threads)
target_link_directories(sonora_core PUBLIC ${JACK_LIBRARY_DIRS})

add_executable(sonora-engine src/main.cpp)
target_link_libraries(sonora-engine PRIVATE sonora_core)

install(TARGETS sonora-engine RUNTIME DESTINATION bin)

enable_testing()
add_executable(test_ringbuffer tests/test_ringbuffer.cpp)
target_link_libraries(test_ringbuffer PRIVATE sonora_core)
add_test(NAME ringbuffer COMMAND test_ringbuffer)

add_executable(test_wav_writer tests/test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE sonora_core)
add_test(NAME wav_writer COMMAND test_wav_writer)
