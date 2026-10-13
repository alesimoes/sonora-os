# The backend with no audio server behind it (ADR-0007 §4.2, M3): engine
# tests, `sonora-engine --backend fake`, and the UI squad's inner loop run on
# it. Platform-neutral (ADR-0008): standard C++17, std::thread included, and
# only the neutral headers of drivers/support.
add_library(sonora_backend_fake STATIC src/fake_backend.cpp)
target_include_directories(sonora_backend_fake PUBLIC include)
target_link_libraries(sonora_backend_fake
  PUBLIC sonora_backend_api sonora_base
  PRIVATE sonora_driver_support Threads::Threads)

add_executable(test_fake_backend tests/test_fake_backend.cpp)
target_link_libraries(test_fake_backend PRIVATE sonora_backend_fake Threads::Threads)
add_test(NAME fake_backend COMMAND test_fake_backend)

# The conformance suite against the fake, in every shape the suite's
# branches need: one USB interface, onboard only, both, two interfaces, the
# injected naming and ordering, and the threaded drive.
add_executable(test_conformance_fake tests/test_conformance_fake.cpp)
target_link_libraries(test_conformance_fake PRIVATE
  sonora_backend_fake sonora_backend_conformance Threads::Threads)
add_test(NAME backend_conformance_fake COMMAND test_conformance_fake)
