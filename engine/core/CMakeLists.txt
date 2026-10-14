# Session::process() and everything it calls: the real-time path. Does not
# link contracts/api; the core knows nothing about the wire protocol. Links
# contracts/backend because Session implements ProcessCallback, and no
# driver: a backend reaches the session only through that interface.
add_library(sonora_core STATIC
  src/session.cpp
  src/track.cpp
  src/disk_thread.cpp
  src/playback_plan.cpp
  src/plan_graph.cpp
  src/plan_exchange.cpp
  src/disk_reader.cpp
  src/plugin_chain_runner.cpp
)
target_include_directories(sonora_core PUBLIC include)
# engine/plugin_client: ADR-0007's edge engine/core -> engine/plugin_client.
# PluginChainRunner calls its audio-thread half (ADR-0012 §7.2); the core
# links no plugin SDK and starts no host.
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

# PluginChainRunner's own cases, no host: a node with no node state plays
# dry, a chain wider than stereo is silence, a mono chain fills both sides.
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
  target_link_libraries(test_session_plugins PRIVATE sonora_core sonora_plugin_client)
  add_dependencies(test_session_plugins sonora-plugin-host sonora_test_lv2)
  set(_sp $<TARGET_FILE:test_session_plugins> --host $<TARGET_FILE:sonora-plugin-host>
          --bundle ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
  set(_sp_tests)
  foreach(scenario aligned_lv2 aligned_2048 stall)
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
