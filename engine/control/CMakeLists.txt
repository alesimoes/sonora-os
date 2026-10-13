# The only module where engine/core and contracts/api meet.
add_library(sonora_control STATIC
  src/session_commands.cpp
  src/state_publisher.cpp
  src/engine_control_server.cpp
  src/engine_control_in_process.cpp
  src/control_socket_server.cpp
)
target_include_directories(sonora_control PUBLIC include)
# common/platform for the control socket and the memfd (ADR-0008 P9): no
# socket or mapping code in this module itself.
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api sonora_platform
  PRIVATE sonora_model)

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
