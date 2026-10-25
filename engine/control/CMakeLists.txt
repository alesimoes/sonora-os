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
  src/orphan_offer.cpp
  src/plugin_hosting.cpp
  src/plugin_scanner.cpp
  src/plugin_catalog_cache.cpp
  src/session_saver.cpp
)
target_include_directories(sonora_control PUBLIC include)
# common/platform for the control socket and the memfd (ADR-0008 P9): no
# socket or mapping code in this module itself. common/model is public: the
# plan builder and closed_take.h take a model::Document in their signatures.
# engine/plugin_client directly, its control-thread half only (the
# supervisor, PluginClient's control API): the edge ADR-0012's amendment of
# 2026-10-14 (change E) draws into the ADR-0007 graph. The RT half is
# reached only through engine/core's PluginChainRunner. common/json reads
# the probes' answers. common/session_files is what the saver thread writes
# a session folder with (ADR-0015 update of 2026-10-25).
target_link_libraries(sonora_control PUBLIC sonora_core sonora_api sonora_platform
  sonora_model sonora_session_files sonora_plugin_client sonora_plugin_probe_record PRIVATE sonora_json)

# The saver thread (ADR-0015 §6, update of 2026-10-25): syncs only after a
# checkpoint while a take is open, cleanup after the transport stops.
if(UNIX)
  add_executable(test_session_saver tests/test_session_saver.cpp)
  target_include_directories(test_session_saver PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_saver PRIVATE sonora_control sonora_sha256 Threads::Threads)
  add_test(NAME session_saver COMMAND test_session_saver)
endif()

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

# A loop take into the document (ADR-0016 §3, §5): its passes on playlists,
# the crossfades it has the handle for, lost wraps placed by loop length,
# the pass cap, and the record commands' loop and punch refusal.
add_executable(test_loop_take tests/test_loop_take.cpp)
target_link_libraries(test_loop_take PRIVATE sonora_control)
add_test(NAME loop_take COMMAND test_loop_take)

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

# The plugin scan worker and its cache (ADR-0012, amendment of 2026-10-19),
# against a fake host that answers --version, --list and --probe from a
# world file: no plugin SDK needed, so it runs in every build.
if(UNIX)
  add_executable(fake_plugin_host tests/fake_plugin_host.cpp)
  target_link_libraries(fake_plugin_host PRIVATE sonora_plugin_probe_record)
  add_executable(test_plugin_scanner tests/test_plugin_scanner.cpp)
  target_link_libraries(test_plugin_scanner PRIVATE sonora_control Threads::Threads)
  add_dependencies(test_plugin_scanner fake_plugin_host)
  add_test(NAME plugin_scanner COMMAND test_plugin_scanner --host $<TARGET_FILE:fake_plugin_host>)
  # The catalog through the control server: item 6's resolution, the
  # catalog sent, plugin.rescan refused during a take.
  add_executable(test_plugin_catalog tests/test_plugin_catalog.cpp)
  target_link_libraries(test_plugin_catalog PRIVATE sonora_control sonora_backend_fake Threads::Threads)
  add_dependencies(test_plugin_catalog fake_plugin_host)
  add_test(NAME plugin_catalog COMMAND test_plugin_catalog --host $<TARGET_FILE:fake_plugin_host>)
  # A real cold and warm scan, timed by hand against the real host (never
  # run by ctest: minutes, and the machine's figure).
  add_executable(measure_plugin_scan tests/measure_plugin_scan.cpp)
  target_link_libraries(measure_plugin_scan PRIVATE sonora_control)
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
                   repost_bypass_off gesture_left_open failed_edit state_not_restored work_refused touch_faults work_refused_new_session server_work_refused play_waits play_wait_bounded move_off_missing
                   play_no_hosting play_wait_cancel rescan_drops_stale rescan_loses_running
                   rescan_blocks_running state_flush server_state_flush)
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

# Takes on disk the open session does not name: what is offered, and how
# placing it goes in (session-format.md, "Crash recovery").
add_executable(test_orphan_offer tests/test_orphan_offer.cpp)
target_link_libraries(test_orphan_offer PRIVATE sonora_control)
add_test(NAME orphan_offer COMMAND test_orphan_offer)
