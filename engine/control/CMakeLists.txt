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
  src/plugin_hosting.cpp
)
target_include_directories(sonora_control PUBLIC include)
# common/platform for the control socket and the memfd (ADR-0008 P9): no
# socket or mapping code in this module itself. common/model is public: the
# plan builder and closed_take.h take a model::Document in their signatures.
# engine/plugin_client directly, its control-thread half only (the
# supervisor, PluginClient's control API): the edge ADR-0012's amendment of
# 2026-10-14 (change E) draws into the ADR-0007 graph. The RT half is
# reached only through engine/core's PluginChainRunner. common/json reads
# the probes' answers.
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api sonora_platform
  sonora_model sonora_plugin_client PRIVATE sonora_json)

# Here rather than in engine/core because it also drives the session through
# the installed commands, and control may link core but not the reverse.
add_executable(test_takes tests/test_takes.cpp)
target_link_libraries(test_takes PRIVATE sonora_control)
add_test(NAME takes COMMAND test_takes)

# A closed take as one compound; a bad file costs one track (ADR-0009
# update 2026-10-14, item 8).
add_executable(test_closed_take tests/test_closed_take.cpp)
target_link_libraries(test_closed_take PRIVATE sonora_control)
add_test(NAME closed_take COMMAND test_closed_take)

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

# PluginHosting against real hosts (ADR-0012 checklist 5): built where the
# host is (tools/build-plugins.sh, CI's plugins job).
if(TARGET sonora-plugin-host)
  add_executable(test_plugin_hosting tests/test_plugin_hosting.cpp)
  target_link_libraries(test_plugin_hosting PRIVATE sonora_control sonora_backend_fake)
  add_dependencies(test_plugin_hosting sonora-plugin-host sonora_test_lv2)
  set(_ph $<TARGET_FILE:test_plugin_hosting> --host $<TARGET_FILE:sonora-plugin-host>
          --bundle ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
  set(_ph_tests)
  foreach(scenario open_starts_silent new_insert_dry edit_keeps_old undo_abandons missing refused params
                   down_keeps_latency gestures server values_while_starting reload_starts_afresh
                   repost_bypass_off gesture_left_open failed_edit state_not_restored play_waits play_wait_bounded
                   play_no_hosting play_wait_cancel)
    add_test(NAME plugin_hosting_${scenario} COMMAND ${_ph} --scenario ${scenario})
    list(APPEND _ph_tests plugin_hosting_${scenario})
  endforeach()
  set_tests_properties(${_ph_tests} PROPERTIES LABELS plugins RUN_SERIAL TRUE)
  if(SONORA_PLUGIN_TESTS_ALLOW_SKIP)
    set_tests_properties(${_ph_tests} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

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
