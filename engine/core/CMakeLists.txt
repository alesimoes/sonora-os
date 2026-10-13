# Session::process() and everything it calls: the real-time path. Does not
# link contracts/api; the core knows nothing about the wire protocol. Links
# contracts/backend because Session implements ProcessCallback, and no
# driver: a backend reaches the session only through that interface.
add_library(sonora_core STATIC
  src/session.cpp
  src/track.cpp
  src/disk_thread.cpp
  src/playback_plan.cpp
  src/plan_exchange.cpp
)
target_include_directories(sonora_core PUBLIC include)
target_link_libraries(sonora_core PUBLIC
  sonora_base sonora_backend_api sonora_audiofile Threads::Threads)

add_executable(test_session_process tests/test_session_process.cpp)
target_link_libraries(test_session_process PRIVATE sonora_core)
add_test(NAME session_process COMMAND test_session_process)

# ADR-0007 §7: the core tests against drivers/fake, the backend's own fake,
# never a private mock of it. A test-only edge; sonora_core itself links no
# driver.
add_executable(test_record_fake tests/test_record_fake.cpp)
target_link_libraries(test_record_fake PRIVATE sonora_core sonora_backend_fake)
add_test(NAME record_fake COMMAND test_record_fake)

# ADR-0009 §4.1's required stress test; CI runs it under TSan.
add_executable(test_plan_exchange tests/test_plan_exchange.cpp)
target_link_libraries(test_plan_exchange PRIVATE sonora_core)
add_test(NAME plan_exchange COMMAND test_plan_exchange)
