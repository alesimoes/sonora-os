# Session::process() and everything it calls: the real-time path. Does not
# link contracts/api; the core knows nothing about the wire protocol.
add_library(sonora_core STATIC
  src/session.cpp
  src/track.cpp
  src/disk_thread.cpp
)
target_include_directories(sonora_core PUBLIC include)
target_link_libraries(sonora_core PUBLIC sonora_base sonora_audiofile Threads::Threads)

add_executable(test_session_process tests/test_session_process.cpp)
target_link_libraries(test_session_process PRIVATE sonora_core)
add_test(NAME session_process COMMAND test_session_process)
