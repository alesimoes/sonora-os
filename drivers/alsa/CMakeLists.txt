# The backend that talks to the card with no server (ADR-0053). The logic
# (threading, xrun recovery, conversion, device choice) is built and tested
# everywhere on Linux against a fake card; only alsa_system.cpp needs
# libasound, and a build without its headers links a stub so the rest still
# compiles and AlsaBackend refuses to open with a message.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)

if(ALSA_FOUND)
  set(SONORA_ALSA_SYSTEM src/alsa_system.cpp)
else()
  message(STATUS "libasound headers not found: sonora_backend_alsa links a stub")
  set(SONORA_ALSA_SYSTEM src/alsa_system_stub.cpp)
endif()

add_library(sonora_backend_alsa STATIC src/alsa_backend.cpp ${SONORA_ALSA_SYSTEM})
target_include_directories(sonora_backend_alsa PUBLIC include)
target_link_libraries(sonora_backend_alsa
  PUBLIC sonora_backend_api sonora_base
  PRIVATE sonora_driver_support Threads::Threads)
if(ALSA_FOUND)
  target_link_libraries(sonora_backend_alsa PRIVATE PkgConfig::ALSA)
endif()

add_executable(test_alsa_convert tests/test_alsa_convert.cpp)
target_link_libraries(test_alsa_convert PRIVATE sonora_backend_alsa)
add_test(NAME alsa_convert COMMAND test_alsa_convert)

add_executable(test_alsa_proc tests/test_alsa_proc.cpp)
target_link_libraries(test_alsa_proc PRIVATE sonora_backend_alsa)
add_test(NAME alsa_proc COMMAND test_alsa_proc)

# The backend and the conformance suite against the fake card.
add_executable(test_alsa_backend tests/test_alsa_backend.cpp)
target_include_directories(test_alsa_backend PRIVATE testing)
target_link_libraries(test_alsa_backend PRIVATE
  sonora_backend_alsa sonora_backend_conformance Threads::Threads)
add_test(NAME alsa_backend COMMAND test_alsa_backend)
set_tests_properties(alsa_backend PROPERTIES TIMEOUT 120)

# The backend on a real ALSA device with the engine's callback replaced by a
# passthrough: tools/test-alsa-loopback.sh runs it against snd-aloop. Needs
# libasound and a loaded module, so it is not a ctest.
if(ALSA_FOUND)
  add_executable(sonora-alsa-passthrough tests/alsa_passthrough.cpp)
  target_link_libraries(sonora-alsa-passthrough PRIVATE sonora_backend_alsa Threads::Threads)
endif()

add_subdirectory(../clock clock)
