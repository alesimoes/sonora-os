# The only target that may include <jack/jack.h>. It does so in src/ only, and
# links JACK privately, so nothing that links this compiles against JACK.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_library(sonora_backend_jack STATIC src/jack_backend.cpp)
target_include_directories(sonora_backend_jack PUBLIC include)
target_link_libraries(sonora_backend_jack
  PUBLIC sonora_backend_api sonora_base
  PRIVATE sonora_driver_support PkgConfig::JACK)

# A stand-in for onboard audio in the container test; see the file. Built
# with the engine so the test can mount it alongside; never installed.
add_executable(sonora-fake-capture testing/fake_capture.cpp)
target_link_libraries(sonora-fake-capture PRIVATE PkgConfig::JACK)

# The backend conformance suite against this backend and a live server.
# Needs one, so it is not a ctest: tools/test-engine-entrypoint.sh runs it
# against PipeWire in the runtime container, with the faults it can cause.
add_executable(sonora-conformance-jack tests/conformance_jack.cpp)
target_link_libraries(sonora-conformance-jack PRIVATE
  sonora_backend_jack sonora_backend_conformance)

# The control for tools/rtsan-jack.sh, which runs JackBackend under RTSan
# against live servers: a callback that allocates, which RTSan must report
# from process_cb. Only in the SONORA_RTSAN build; not a ctest, for the same
# reason as the conformance run.
if(SONORA_RTSAN)
  add_executable(sonora-rtsan-jack-canary tests/rtsan_canary.cpp)
  target_link_libraries(sonora-rtsan-jack-canary PRIVATE sonora_backend_jack)
endif()
