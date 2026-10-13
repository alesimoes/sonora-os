# The only module where engine/core and contracts/api meet. Also where the
# model meets the engine: the plan builder and its publisher (ADR-0007 §2,
# ADR-0009 §4).
add_library(sonora_control STATIC
  src/session_commands.cpp
  src/state_publisher.cpp
  src/engine_control_server.cpp
  src/engine_control_in_process.cpp
  src/control_socket_server.cpp
  src/plan_builder.cpp
  src/playback_controller.cpp
  src/closed_take.cpp
)
target_include_directories(sonora_control PUBLIC include)
# common/platform for the control socket and the memfd (ADR-0008 P9): no
# socket or mapping code in this module itself. common/model is public: the
# plan builder and closed_take.h take a model::Document in their signatures.
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api sonora_platform
  sonora_model)

# Here rather than in engine/core because it also drives the session through
# the installed commands, and control may link core but not the reverse.
add_executable(test_takes tests/test_takes.cpp)
target_link_libraries(test_takes PRIVATE sonora_control)
add_test(NAME takes COMMAND test_takes)

# The handlers on a real Session (moved here from ui/keymap's test in M4).
add_executable(test_session_commands tests/test_session_commands.cpp)
target_link_libraries(test_session_commands PRIVATE sonora_control)
add_test(NAME session_commands COMMAND test_session_commands)

# The control API end to end on the real engine: the conformance suite on
# InProcessEngine over drivers/fake (a test-only edge, as engine/core's
# test_record_fake has), the publisher's pacing, and FakeEngine's command
# list checked against the handlers this build installs.
add_executable(test_control tests/test_control.cpp)
target_link_libraries(test_control PRIVATE
  sonora_control sonora_backend_fake sonora_api_conformance sonora_api_fake sonora_model)
add_test(NAME control COMMAND test_control)

# The server on a socket (M7), driven by raw platform channels rather than
# ui/client, so this module's test needs nothing above it: several
# clients, a client gone mid-take, malformed frames refused, the state
# block arriving read-only.
if(UNIX)
  add_executable(test_control_socket tests/test_control_socket.cpp)
  target_link_libraries(test_control_socket PRIVATE sonora_control sonora_backend_fake)
  add_test(NAME control_socket COMMAND test_control_socket)
endif()

add_executable(test_playback_control tests/test_playback_control.cpp)
target_link_libraries(test_playback_control PRIVATE sonora_control)
add_test(NAME playback_control COMMAND test_playback_control)

# Record then play back against drivers/fake (ADR-0007 §7): a test-only
# edge, as in engine/core; sonora_control links no driver.
add_executable(test_playback_fake tests/test_playback_fake.cpp)
target_link_libraries(test_playback_fake PRIVATE sonora_control sonora_backend_fake)
add_test(NAME playback_fake COMMAND test_playback_fake)

# The docker PLAYBACK check's loopback in process, under jittered periods
# and the reader and control threads running free.
add_executable(test_playback_loopback tests/test_playback_loopback.cpp)
target_link_libraries(test_playback_loopback PRIVATE
  sonora_control sonora_backend_fake Threads::Threads)
add_test(NAME playback_loopback COMMAND test_playback_loopback)

# ADR-0009's request-to-start latency and plan build time, measured by hand
# (the figures depend on the machine, and a run takes a minute): built with
# the tests so it cannot rot, never run by ctest.
add_executable(measure_playback tests/measure_playback.cpp)
target_link_libraries(measure_playback PRIVATE sonora_control sonora_backend_fake)
