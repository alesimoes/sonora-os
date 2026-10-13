# sonora-plugin-host (ADR-0007 §1, §3, Phase 4): runs LV2 plugins in a
# process of their own. The only target that links a plugin SDK (lilv).
# An edge module under ADR-0008: POSIX and Linux code is allowed here, kept
# under src/linux/ behind the neutral src/ipc.h.
#
# Optional by construction, like ui/qt: the engine's build container and CI
# have no lilv, and they must stay green. tools/build-plugins.sh builds and
# tests this in the sonora-plugins image, which has lilv and the LV2
# plugins the tests use.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "plugins/host: Linux only for now; sonora-plugin-host is skipped")
  return()
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
endif()
if(NOT LILV_FOUND)
  message(STATUS "plugins/host: lilv-0 not found; sonora-plugin-host and its tests are skipped (tools/build-plugins.sh builds them)")
  return()
endif()
message(STATUS "plugins/host: building sonora-plugin-host against lilv ${LILV_VERSION}")

# The seam both sides of the boundary use: the host here, and the engine-role
# test driver. The engine's half moves to common/platform with M4+ (ipc.h).
add_library(sonora_plugin_ipc STATIC src/ipc.h src/linux/ipc_linux.cpp)
target_include_directories(sonora_plugin_ipc PUBLIC src)
target_link_libraries(sonora_plugin_ipc PUBLIC sonora_plugin_protocol)

# The URID map plugins call from run() on the SCHED_FIFO thread: lock-free
# and allocation-free after construction. Needs only the LV2 headers.
add_library(sonora_urid_map STATIC src/urid_map.h src/urid_map.cpp)
target_include_directories(sonora_urid_map PUBLIC src)
target_link_libraries(sonora_urid_map PUBLIC PkgConfig::LILV)

add_executable(sonora-plugin-host src/main.cpp src/lv2_host.cpp src/lv2_host.h)
target_link_libraries(sonora-plugin-host PRIVATE sonora_plugin_ipc sonora_urid_map PkgConfig::LILV)

# --- tests (ADR-0007 §7: a fake engine driver plus in-tree test plugins)

find_package(Threads REQUIRED)
add_executable(test_urid_map tests/test_urid_map.cpp)
target_link_libraries(test_urid_map PRIVATE sonora_urid_map Threads::Threads)
add_test(NAME plugin_host_urid_map COMMAND test_urid_map)
set_tests_properties(plugin_host_urid_map PROPERTIES LABELS plugins)

# The misbehaving plugins, as an LV2 bundle in the build tree. Never installed.
set(SONORA_TEST_BUNDLE ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
add_library(sonora_test_lv2 MODULE tests/lv2/sonora_test.cpp)
target_link_libraries(sonora_test_lv2 PRIVATE PkgConfig::LILV)
set_target_properties(sonora_test_lv2 PROPERTIES
  PREFIX ""
  OUTPUT_NAME sonora_test
  LIBRARY_OUTPUT_DIRECTORY ${SONORA_TEST_BUNDLE}
  CXX_VISIBILITY_PRESET hidden)
configure_file(tests/lv2/manifest.ttl ${SONORA_TEST_BUNDLE}/manifest.ttl COPYONLY)
configure_file(tests/lv2/sonora_test.ttl ${SONORA_TEST_BUNDLE}/sonora_test.ttl COPYONLY)

add_executable(plugin_driver tests/plugin_driver.cpp)
target_link_libraries(plugin_driver PRIVATE sonora_plugin_ipc)
add_dependencies(plugin_driver sonora-plugin-host sonora_test_lv2)

set(_drive $<TARGET_FILE:plugin_driver> --host $<TARGET_FILE:sonora-plugin-host>
           --bundle ${SONORA_TEST_BUNDLE})

# Plugins from Debian (tools/Dockerfile.plugins): eg-amp from lv2-examples,
# x42-dpl from x42-plugins, the LSP limiter from lsp-plugins-lv2. A test whose
# plugin is not installed exits 77. That is a FAILURE unless skips are
# explicitly allowed: CI must not report green for a test that never ran.
option(SONORA_PLUGIN_TESTS_ALLOW_SKIP
       "Report a plugin test whose Debian plugin is missing as skipped, not failed" OFF)
add_test(NAME plugin_host_list COMMAND sonora-plugin-host --list --json --bundle ${SONORA_TEST_BUNDLE})
set_tests_properties(plugin_host_list PROPERTIES PASS_REGULAR_EXPRESSION "\"uri\":\"urn:sonora:test#crash\"")
# --probe refuses the liar, from a relative bundle path (a hand-built file
# URI once turned lv2/x.lv2 into file://lv2/x.lv2 and found nothing).
add_test(NAME plugin_host_probe_liar
         COMMAND sonora-plugin-host --probe "urn:sonora:test#liar" --bundle lv2/sonora-test.lv2
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_host_probe_liar PROPERTIES PASS_REGULAR_EXPRESSION "\"ok\":false,\"reason\":\"[^\"]*wrote outside them")
# A host whose engine is already gone exits before it maps anything: the
# death signal requested after the engine died would never fire.
add_test(NAME plugin_host_orphan
         COMMAND sonora-plugin-host --shm-fd 0 --chain "urn:sonora:test#crash" --engine-pid 999999)
set_tests_properties(plugin_host_orphan PROPERTIES PASS_REGULAR_EXPRESSION "is not this host.s parent"
                     LABELS plugins)
set(_plugin_tests
  plugin_host_refusals
  plugin_host_crash plugin_host_crash_poll
  plugin_host_deadline plugin_host_deadline_poll
  plugin_host_nonfinite
  plugin_host_gain_mono plugin_host_gain_stereo plugin_host_gain_poll
  plugin_host_latency_pipelined plugin_host_latency_poll plugin_host_latency_chain
  plugin_host_rtt)
add_test(NAME plugin_host_refusals COMMAND ${_drive} --scenario refusals)
# pipeline_depth 1 is the default mode; poll is depth 0, poll-only.
add_test(NAME plugin_host_crash COMMAND ${_drive} --scenario crash)
add_test(NAME plugin_host_crash_poll COMMAND ${_drive} --scenario crash --mode poll)
add_test(NAME plugin_host_deadline COMMAND ${_drive} --scenario deadline)
add_test(NAME plugin_host_deadline_poll COMMAND ${_drive} --scenario deadline --mode poll)
add_test(NAME plugin_host_nonfinite COMMAND ${_drive} --scenario nonfinite)
add_test(NAME plugin_host_gain_mono COMMAND ${_drive} --scenario gain)
add_test(NAME plugin_host_gain_stereo COMMAND ${_drive} --scenario gain --channels 2)
add_test(NAME plugin_host_gain_poll COMMAND ${_drive} --scenario gain --mode poll)
add_test(NAME plugin_host_latency_pipelined COMMAND ${_drive} --scenario latency)
add_test(NAME plugin_host_latency_poll COMMAND ${_drive} --scenario latency --mode poll)
add_test(NAME plugin_host_latency_chain COMMAND ${_drive} --scenario latency
         --chain "http://gareus.org/oss/lv2/dpl#mono,http://lsp-plug.in/plugins/lv2/limiter_mono,http://lv2plug.in/plugins/eg-amp")
add_test(NAME plugin_host_rtt COMMAND ${_drive} --scenario rtt --blocks 2000)
set_tests_properties(${_plugin_tests} PROPERTIES LABELS plugins RUN_SERIAL TRUE)
if(SONORA_PLUGIN_TESTS_ALLOW_SKIP)
  set_tests_properties(${_plugin_tests} PROPERTIES SKIP_RETURN_CODE 77)
endif()
