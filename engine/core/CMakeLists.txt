# Session::process() and everything it calls: the real-time path. Does not
# link contracts/api; the core knows nothing about the wire protocol. Links
# contracts/backend because Session implements ProcessCallback, and no
# driver: a backend reaches the session only through that interface.
# PluginChainRunner on its own, an OBJECT library like
# sonora_plugin_client_rt, so engine/plugin_client's A3 reachability test
# can hold its object to the audio thread's allowlist.
add_library(sonora_core_chain_runner OBJECT src/plugin_chain_runner.cpp)
target_include_directories(sonora_core_chain_runner PUBLIC include)
target_link_libraries(sonora_core_chain_runner PUBLIC sonora_base sonora_plugin_client)

add_library(sonora_core STATIC
  src/session.cpp
  src/click.cpp
  src/latency_probe.cpp
  src/track.cpp
  src/disk_thread.cpp
  src/loop_take.cpp
  src/playback_plan.cpp
  src/plan_graph.cpp
  src/plan_exchange.cpp
  src/disk_reader.cpp
  $<TARGET_OBJECTS:sonora_core_chain_runner>
)
target_include_directories(sonora_core PUBLIC include)
# engine/plugin_client: ADR-0007's edge engine/core -> engine/plugin_client.
# PluginChainRunner calls its audio-thread half (ADR-0012 §7.2); the core
# links no plugin SDK and starts no host. sonora_plugin_client links
# common/platform PRIVATE, so this edge does not open engine/core ->
# common/platform (ADR-0007 §1): a platform include here fails to compile.
target_link_libraries(sonora_core PUBLIC
  sonora_base sonora_backend_api sonora_audiofile sonora_plugin_client Threads::Threads)

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

add_executable(test_disk_reader tests/test_disk_reader.cpp)
target_link_libraries(test_disk_reader PRIVATE sonora_core)
add_test(NAME disk_reader COMMAND test_disk_reader)

add_executable(test_playback_mix tests/test_playback_mix.cpp)
target_link_libraries(test_playback_mix PRIVATE sonora_core)
add_test(NAME playback_mix COMMAND test_playback_mix)

# ADR-0012 checklist 3: insert chains and PDC in the plan. The 2048-frame
# alignment on a test runner with a declared latency, the clamp and the
# delay-line pool cap, a latency change by swap, no allocation in process().
add_executable(test_plan_pdc tests/test_plan_pdc.cpp)
target_link_libraries(test_plan_pdc PRIVATE sonora_core)
add_test(NAME plan_pdc COMMAND test_plan_pdc)

add_executable(test_sends tests/test_sends.cpp)
target_link_libraries(test_sends PRIVATE sonora_core)
add_test(NAME sends COMMAND test_sends)

# Takes closed while rolling, by in-band boundary records (timeline-model.md,
# "Closing a take while rolling"), and the take-epoch race's regression test.
# CI runs it under TSan.
add_executable(test_take_boundaries tests/test_take_boundaries.cpp)
target_link_libraries(test_take_boundaries PRIVATE sonora_core Threads::Threads)
add_test(NAME take_boundaries COMMAND test_take_boundaries)

# The round trip measured through drivers/fake's loopback cable (ADR-0019):
# the probe's figure to the frame, through the monitored-input path with and
# without PDC, no allocation in process(), and takes compensated by it.
# The click and the count-off (ADR-0020): beats on their frames through a
# locate and a loop, the count-off's length, the take it holds, nothing of
# either in the file, no allocation in process().
add_executable(test_click tests/test_click.cpp)
target_link_libraries(test_click PRIVATE sonora_core)
add_test(NAME click COMMAND test_click)

add_executable(test_latency_probe tests/test_latency_probe.cpp)
target_link_libraries(test_latency_probe PRIVATE sonora_core sonora_backend_fake)
add_test(NAME latency_probe COMMAND test_latency_probe)

# Punch points inside a block (timeline-model.md, "Punch in"): capture opened
# and closed at the frame, Auto monitoring crossfaded at the punch points,
# every other monitor switch ramped.
add_executable(test_punch_points tests/test_punch_points.cpp)
target_link_libraries(test_punch_points PRIVATE sonora_core)
add_test(NAME punch_points COMMAND test_punch_points)

add_executable(test_heap_counter tests/test_heap_counter.cpp)
target_link_libraries(test_heap_counter PRIVATE sonora_core)
add_test(NAME heap_counter COMMAND test_heap_counter)

# Loop record (ADR-0016): one file per track through every wrap, each wrap
# marked in band and kept in the take's wrap list, a lost one keeping its
# place, the pass cap, the short-loop refusals, playback across the wrap.
add_executable(test_loop_record tests/test_loop_record.cpp)
target_link_libraries(test_loop_record PRIVATE sonora_core)
add_test(NAME loop_record COMMAND test_loop_record)

# Loop playback while not recording: the wrap at the sample, a loop shorter
# than the reader's window or longer than its pool, a new range gated on
# its start's residency, and takes that do not wrap or give the hold back.
add_executable(test_loop_playback tests/test_loop_playback.cpp)
target_link_libraries(test_loop_playback PRIVATE sonora_core)
add_test(NAME loop_playback COMMAND test_loop_playback)

# Region fades through process(): every shape in and out, the same output
# however the roll is cut into blocks, a locate into a fade, a loop wrapping
# from a fade out into a fade in, with no allocation in process().
add_executable(test_region_fades tests/test_region_fades.cpp)
target_link_libraries(test_region_fades PRIVATE sonora_core)
add_test(NAME region_fades COMMAND test_region_fades)

# Crossfades through process(): pre, post and centred, every shape, the same
# output however the roll is cut into blocks, a locate into a crossfade, a
# loop wrapping into and out of one, with no allocation in process().
add_executable(test_crossfades tests/test_crossfades.cpp)
target_link_libraries(test_crossfades PRIVATE sonora_core)
add_test(NAME crossfades COMMAND test_crossfades)

# A loop take's passes from its wraps (ADR-0016 §5): lost wraps and a wrap
# list that stops early or is missing derived from the loop length, and
# recovery reading a take's file and list.
add_executable(test_loop_passes tests/test_loop_passes.cpp)
target_link_libraries(test_loop_passes PRIVATE sonora_core)
add_test(NAME loop_passes COMMAND test_loop_passes)

# Post-roll: a roll stops at its end frame, at the sample, never during a
# take, with no allocation in process().
add_executable(test_roll_end tests/test_roll_end.cpp)
target_link_libraries(test_roll_end PRIVATE sonora_core)
add_test(NAME roll_end COMMAND test_roll_end)

# ADR-0012 checklist 5: plugin parameter changes from the control thread to
# the InsertChain node that names their store, the backend's callback_ns as
# the runner's clock, and the runner's block size gating the callback.
add_executable(test_plugin_params tests/test_plugin_params.cpp)
target_link_libraries(test_plugin_params PRIVATE sonora_core)
add_test(NAME plugin_params COMMAND test_plugin_params)

# PluginChainRunner's own cases, no host: a node with no node state is
# silence, a chain wider than stereo is silence, a mono chain fills both
# sides.
add_executable(test_plugin_chain_runner tests/test_plugin_chain_runner.cpp)
target_link_libraries(test_plugin_chain_runner PRIVATE sonora_core)
add_test(NAME plugin_chain_runner COMMAND test_plugin_chain_runner)

# Session::process() running real hosts through PluginChainRunner (ADR-0012
# checklist 4, the DAW 5 exit check against real plugins): tracks behind LV2
# and VST3 chains, Debian's and in-tree, land sample-aligned at the bus with
# a dry track; a 2048-frame chain among them; a host that stops answering
# costs its own track and never the callback. Built where the host is
# (tools/build-plugins.sh, CI's plugins job).
if(TARGET sonora-plugin-host)
  add_executable(test_session_plugins tests/test_session_plugins.cpp)
  # common/platform for the test's own probe and wait, not through the core.
  target_link_libraries(test_session_plugins PRIVATE sonora_core sonora_plugin_client sonora_platform)
  add_dependencies(test_session_plugins sonora-plugin-host sonora_test_lv2)
  set(_sp $<TARGET_FILE:test_session_plugins> --host $<TARGET_FILE:sonora-plugin-host>
          --bundle ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
  set(_sp_tests)
  foreach(scenario aligned_lv2 aligned_2048 aligned_stereo stall params)
    add_test(NAME session_plugins_${scenario} COMMAND ${_sp} --scenario ${scenario})
    list(APPEND _sp_tests session_plugins_${scenario})
  endforeach()
  if(VST3SDK_INCLUDE_DIR)
    add_test(NAME session_plugins_aligned_vst3 COMMAND ${_sp} --scenario aligned_vst3)
    list(APPEND _sp_tests session_plugins_aligned_vst3)
  endif()
  set_tests_properties(${_sp_tests} PROPERTIES LABELS plugins RUN_SERIAL TRUE)
  if(SONORA_PLUGIN_TESTS_ALLOW_SKIP)
    set_tests_properties(${_sp_tests} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# Take safety (BACKLOG DAW 1): the disk thread checkpoints open takes on a
# schedule, and a take survives kill -9 of the recording process. The kill
# needs fork (ADR-0008 V3: OS-specific tests live in tests/posix).
add_executable(test_take_checkpoint tests/test_take_checkpoint.cpp)
target_link_libraries(test_take_checkpoint PRIVATE sonora_core Threads::Threads)
add_test(NAME take_checkpoint COMMAND test_take_checkpoint)
if(UNIX)
  add_executable(test_take_kill tests/posix/test_take_kill.cpp)
  target_link_libraries(test_take_kill PRIVATE sonora_core sonora_audiofile Threads::Threads)
  add_test(NAME take_kill COMMAND test_take_kill)
endif()
