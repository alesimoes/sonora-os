# sonora-plugin-host (ADR-0007 §1, §3, Phase 4): runs LV2 and VST3 plugins in a
# process of their own. The only target that links a plugin SDK (lilv, and
# the VST3 interface headers).
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
# test driver. The wake itself is common/platform's (wake.h), the one the
# engine's audio thread will use (ADR-0012 §1.2).
# start_thread: the host's processing thread.
find_package(Threads REQUIRED)
add_library(sonora_plugin_ipc STATIC src/ipc.h src/linux/ipc_linux.cpp)
target_include_directories(sonora_plugin_ipc PUBLIC src)
target_link_libraries(sonora_plugin_ipc PUBLIC sonora_plugin_protocol sonora_platform Threads::Threads)

# The URID map plugins call from run() on the SCHED_FIFO thread: lock-free
# and allocation-free after construction. Needs only the LV2 headers.
add_library(sonora_urid_map STATIC src/urid_map.h src/urid_map.cpp)
target_include_directories(sonora_urid_map PUBLIC src)
target_link_libraries(sonora_urid_map PUBLIC PkgConfig::LILV)

# VST3: Steinberg's interface headers only (pluginterfaces), no SDK library.
# Debian packages them as libvst3sdk-dev (3.7.10, used under its GPL-3 arm;
# MIT from 3.8, brain/research/plugin-host-vst3.md). Optional like lilv for a
# developer's machine; tools/build-plugins.sh sets SONORA_REQUIRE_VST3 so CI
# fails rather than building a host without VST3 and skipping its tests.
option(SONORA_REQUIRE_VST3 "Fail the configure step when the VST3 SDK headers are missing" OFF)
find_path(VST3SDK_INCLUDE_DIR pluginterfaces/vst/ivstaudioprocessor.h
          PATHS /usr/include/vst3sdk /usr/local/include/vst3sdk)
if(VST3SDK_INCLUDE_DIR)
  message(STATUS "plugins/host: VST3 hosting against ${VST3SDK_INCLUDE_DIR}")
elseif(SONORA_REQUIRE_VST3)
  message(FATAL_ERROR "plugins/host: SONORA_REQUIRE_VST3 is ON and the VST3 SDK headers "
                      "(libvst3sdk-dev) were not found")
else()
  message(STATUS "plugins/host: VST3 SDK headers not found; the host is built LV2-only")
endif()

add_library(sonora_plugin_loader STATIC src/loader.h src/linux/loader_linux.cpp)
target_include_directories(sonora_plugin_loader PUBLIC src)
target_link_libraries(sonora_plugin_loader PUBLIC sonora_plugin_ipc ${CMAKE_DL_LIBS})

add_executable(sonora-plugin-host src/main.cpp src/processor.h src/chain.cpp src/chain.h
               src/lv2_host.cpp src/lv2_host.h src/lv2_worker.cpp src/lv2_worker.h
               src/control.cpp src/control.h)
target_link_libraries(sonora-plugin-host PRIVATE sonora_plugin_ipc sonora_plugin_loader
                      sonora_urid_map sonora_plugin_host_control sonora_plugin_probe_record PkgConfig::LILV)
if(VST3SDK_INCLUDE_DIR)
  target_sources(sonora-plugin-host PRIVATE src/vst3_host.cpp src/vst3_host.h)
  target_include_directories(sonora-plugin-host SYSTEM PRIVATE ${VST3SDK_INCLUDE_DIR})
  target_compile_definitions(sonora-plugin-host PRIVATE SONORA_HAVE_VST3)
endif()

# --- tests (ADR-0007 §7: a fake engine driver plus in-tree test plugins)

# The gesture rules without a plugin (rt-engineer M3 on PR #51).
add_executable(test_gestures tests/test_gestures.cpp)
target_include_directories(test_gestures PRIVATE src)
target_link_libraries(test_gestures PRIVATE sonora_plugin_host_control sonora_platform)
add_test(NAME plugin_host_gestures COMMAND test_gestures)
set_tests_properties(plugin_host_gestures PROPERTIES LABELS plugins)

add_executable(test_urid_map tests/test_urid_map.cpp)
target_link_libraries(test_urid_map PRIVATE sonora_urid_map Threads::Threads)
add_test(NAME plugin_host_urid_map COMMAND test_urid_map)
set_tests_properties(plugin_host_urid_map PROPERTIES LABELS plugins)

# The LV2 worker's request and response ring (worker:schedule).
add_executable(test_lv2_worker tests/test_lv2_worker.cpp src/lv2_worker.cpp)
target_include_directories(test_lv2_worker PRIVATE src)
target_link_libraries(test_lv2_worker PRIVATE sonora_plugin_ipc PkgConfig::LILV Threads::Threads)
add_test(NAME plugin_host_lv2_worker COMMAND test_lv2_worker)
set_tests_properties(plugin_host_lv2_worker PROPERTIES LABELS plugins)

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
target_link_libraries(plugin_driver PRIVATE sonora_plugin_ipc sonora_plugin_host_control)
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
# Every record names its bundle, absolute (ADR-0012, amendment of
# 2026-10-19, item 1), and --bundles-only lists the bundles alone.
add_test(NAME plugin_host_list_bundle COMMAND sonora-plugin-host --list --json --format lv2
         --bundle lv2/sonora-test.lv2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_host_list_bundle PROPERTIES PASS_REGULAR_EXPRESSION
  "\"uri\":\"urn:sonora:test#crash\"[^\n]*\"bundle\":\"/[^\"]*/lv2/sonora-test.lv2\"")
add_test(NAME plugin_host_list_bundles_only COMMAND sonora-plugin-host --list --json --bundles-only --format lv2
         --bundle ${SONORA_TEST_BUNDLE})
set_tests_properties(plugin_host_list_bundles_only PROPERTIES PASS_REGULAR_EXPRESSION
  "{\"bundle\":\"[^\"]*/lv2/sonora-test.lv2\",\"format\":\"lv2\"}\n")
# One probe record, version 2 (contracts/plugin probe_record.h), with the
# description the catalog shows: the parameter's name, range and unit-less
# flags; native, so no "bridge" after the bundle. eg-amp is from Debian's
# lv2-examples.
add_test(NAME plugin_host_probe_record COMMAND sonora-plugin-host --probe http://lv2plug.in/plugins/eg-amp --channels 2)
set_tests_properties(plugin_host_probe_record PROPERTIES PASS_REGULAR_EXPRESSION
  "^{\"record\":2,\"host\":\"sonora-plugin-host layout 1 clock 1 wire 3 probe 2\",\"uri\":\"http://lv2plug.in/plugins/eg-amp\",\"format\":\"lv2\",\"bundle\":\"/[^\"]+\",\"name\":\"[^\"]+\"[^\n]*\"ok\":true,\"channels\":2,\"replicas\":2,[^\n]*\"params\":\\[{\"key\":\"gain\",\"symbol\":\"gain\",\"name\":\"Gain\",\"min\":-90.0,\"max\":24.0,")
add_test(NAME plugin_host_probe_not_installed COMMAND sonora-plugin-host --probe "urn:sonora:test#absent")
set_tests_properties(plugin_host_probe_not_installed PROPERTIES PASS_REGULAR_EXPRESSION
  "\"ok\":false,\"reason\":\"not installed\"[^\n]*\"outcome\":\"not_installed\"")
set_tests_properties(plugin_host_list_bundle plugin_host_list_bundles_only plugin_host_probe_record
  plugin_host_probe_not_installed PROPERTIES LABELS plugins)
# --probe refuses the liar, from a relative bundle path (a hand-built file
# URI once turned lv2/x.lv2 into file://lv2/x.lv2 and found nothing).
add_test(NAME plugin_host_probe_liar
         COMMAND sonora-plugin-host --probe "urn:sonora:test#liar" --bundle lv2/sonora-test.lv2
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_host_probe_liar PROPERTIES PASS_REGULAR_EXPRESSION "\"ok\":false,\"reason\":\"[^\"]*wrote outside them")
# A host whose engine is already gone exits before it maps anything: the
# death signal requested after the engine died would never fire.
add_test(NAME plugin_host_orphan
         COMMAND sonora-plugin-host --shm-fd 0 --clock-fd 0 --chain "urn:sonora:test#crash" --engine-pid 999999)
set_tests_properties(plugin_host_orphan PROPERTIES PASS_REGULAR_EXPRESSION "is not this host.s parent"
                     LABELS plugins)
# The versions the engine checks at start-up (ADR-0012 §5.5).
add_test(NAME plugin_host_version COMMAND sonora-plugin-host --version)
set_tests_properties(plugin_host_version PROPERTIES PASS_REGULAR_EXPRESSION "^sonora-plugin-host layout 1 clock 1 wire 3 probe 2\n$"
                     LABELS plugins)
# Hosts are timer-paced by default: the audio thread's futex wake ships gated
# off (contracts/plugin kHostWakeEnabled, ADR-0012 §1.3-1.4). The *_futex
# tests run the same scenarios on the wake path the gate would turn on.
set(_plugin_tests
  plugin_host_refusals plugin_host_incompatible
  plugin_host_crash plugin_host_crash_futex
  plugin_host_deadline plugin_host_deadline_futex
  plugin_host_nonfinite
  plugin_host_gain_mono plugin_host_gain_stereo plugin_host_gain_futex
  plugin_host_latency_pipelined plugin_host_latency_futex plugin_host_latency_chain
  plugin_host_bypass plugin_host_bypass_latency plugin_host_bypass_lv2_enabled plugin_host_swap
  plugin_host_rtt plugin_host_key
  plugin_host_control plugin_host_control_incompatible plugin_host_control_gain plugin_host_control_crash
  plugin_host_control_latency_chain plugin_host_state_lv2
  plugin_host_worker plugin_host_worker_futex plugin_host_worker_control plugin_host_worker_restore
  plugin_host_worker_hang plugin_host_worker_slow_exit plugin_host_worker_flood)
# HostControl (contracts/plugin host_control.h, ADR-0012 §4): the socket on
# --control-fd, with the driver as the engine's control thread. The same
# scenarios as above through Load instead of the command line, and the
# messages only the socket carries: the handshake and its refusals, the key
# table, DescribeValue, editors, state save and restore through a
# "Plugin State/<sha256>.state" file, Shutdown.
add_test(NAME plugin_host_control COMMAND ${_drive} --scenario control)
add_test(NAME plugin_host_control_incompatible COMMAND ${_drive} --scenario control_incompatible)
add_test(NAME plugin_host_control_gain COMMAND ${_drive} --scenario gain --control 1)
add_test(NAME plugin_host_control_crash COMMAND ${_drive} --scenario crash --control 1)
add_test(NAME plugin_host_control_latency_chain COMMAND ${_drive} --scenario latency --control 1
         --chain "http://gareus.org/oss/lv2/dpl#mono,http://lsp-plug.in/plugins/lv2/limiter_mono,http://lv2plug.in/plugins/eg-amp")
add_test(NAME plugin_host_state_lv2 COMMAND ${_drive} --scenario state --chain "urn:sonora:test#state"
         --expect-state counter)
add_test(NAME plugin_host_refusals COMMAND ${_drive} --scenario refusals)
add_test(NAME plugin_host_incompatible COMMAND ${_drive} --scenario incompatible)
# The key channels: re-copied for every plugin, and a key input with no key
# routed reads the plugin's own input.
add_test(NAME plugin_host_key COMMAND ${_drive} --scenario key)
add_test(NAME plugin_host_crash COMMAND ${_drive} --scenario crash)
add_test(NAME plugin_host_crash_futex COMMAND ${_drive} --scenario crash --wake futex)
add_test(NAME plugin_host_deadline COMMAND ${_drive} --scenario deadline)
add_test(NAME plugin_host_deadline_futex COMMAND ${_drive} --scenario deadline --wake futex)
add_test(NAME plugin_host_nonfinite COMMAND ${_drive} --scenario nonfinite)
# LV2 worker:schedule: work() off the processing thread, its two responses
# handed back inside run() and end_run() after it, on both wakes and through
# Load; and a schedule from restore(), answered in the first run().
add_test(NAME plugin_host_worker COMMAND ${_drive} --scenario worker)
add_test(NAME plugin_host_worker_futex COMMAND ${_drive} --scenario worker --wake futex)
add_test(NAME plugin_host_worker_control COMMAND ${_drive} --scenario worker --control 1)
add_test(NAME plugin_host_worker_restore COMMAND ${_drive} --scenario worker_restore)
# A work() that never returns: the Reconfigure after it is answered with a
# Fault (about 9 s, the engine's bound less a second) and the host exits.
add_test(NAME plugin_host_worker_hang COMMAND ${_drive} --scenario worker_hang)
# A work() that returns 1.5 s after the stopping host abandons it: the host
# has already exited Faulted, its destructors not run.
add_test(NAME plugin_host_worker_slow_exit COMMAND ${_drive} --scenario worker_slow_exit)
# Requests past the worker's ring, every block: said as Fault{WorkRefused},
# at most once a second.
add_test(NAME plugin_host_worker_flood COMMAND ${_drive} --scenario worker_flood)
# The RTSan build only: the host's block (run_block) is SONORA_RT, so the
# plugins it runs are checked as the engine's callback is. The control for
# that: the deadline scenario with the test plugin's sleep left unexempted
# must be reported from run_block. Its pass condition is the report, so
# tools/build-rtsan.sh's scan for reports in passing tests skips it.
if(SONORA_RTSAN)
  add_test(NAME plugin_host_rtsan_block COMMAND ${_drive} --scenario deadline)
  set_tests_properties(plugin_host_rtsan_block PROPERTIES
    ENVIRONMENT "SONORA_TEST_RTSAN_SLEEP=1;RTSAN_OPTIONS=halt_on_error=true"
    PASS_REGULAR_EXPRESSION "real-time unsafe function `nanosleep`.*run_block"
    LABELS "plugins;rtsan-report")
endif()
add_test(NAME plugin_host_gain_mono COMMAND ${_drive} --scenario gain)
add_test(NAME plugin_host_gain_stereo COMMAND ${_drive} --scenario gain --channels 2)
add_test(NAME plugin_host_gain_futex COMMAND ${_drive} --scenario gain --wake futex)
add_test(NAME plugin_host_latency_pipelined COMMAND ${_drive} --scenario latency)
add_test(NAME plugin_host_latency_futex COMMAND ${_drive} --scenario latency --wake futex)
add_test(NAME plugin_host_latency_chain COMMAND ${_drive} --scenario latency
         --chain "http://gareus.org/oss/lv2/dpl#mono,http://lsp-plug.in/plugins/lv2/limiter_mono,http://lv2plug.in/plugins/eg-amp")
# kBypassParam: host-side (delay line) at latency 0 and with a look-ahead
# (the in-tree lookahead plugin has no bypass port), and through a plugin's
# own lv2:enabled port (x42-dpl declares one).
add_test(NAME plugin_host_bypass COMMAND ${_drive} --scenario bypass --chain http://lv2plug.in/plugins/eg-amp
         --gain-param gain --expect-bypass host)
add_test(NAME plugin_host_bypass_latency COMMAND ${_drive} --scenario bypass
         --chain "urn:sonora:test#lookahead" --gain-param gain --expect-bypass host)
add_test(NAME plugin_host_bypass_lv2_enabled COMMAND ${_drive} --scenario bypass
         --chain "http://gareus.org/oss/lv2/dpl#mono" --expect-bypass plugin)
# Make-before-break into a new region and slot (ADR-0012 §5.3, §5.4).
add_test(NAME plugin_host_swap COMMAND ${_drive} --scenario swap)
add_test(NAME plugin_host_rtt COMMAND ${_drive} --scenario rtt --blocks 2000)

# --- VST3 (brain/research/plugin-host-vst3.md). Plugins from Debian
# (tools/Dockerfile.plugins): AGain from vst3sdk-plugins, the LSP limiter from
# lsp-plugins-vst3; plus the misbehaving in-tree bundle below.
if(VST3SDK_INCLUDE_DIR)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_vst3_arch x86_64-linux)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_vst3_arch aarch64-linux)
  else()
    set(_vst3_arch ${CMAKE_SYSTEM_PROCESSOR}-linux)
  endif()
  set(SONORA_TEST_VST3 ${CMAKE_BINARY_DIR}/vst3/sonora-test.vst3)
  add_library(sonora_test_vst3 MODULE tests/vst3/sonora_test_vst3.cpp)
  target_include_directories(sonora_test_vst3 SYSTEM PRIVATE ${VST3SDK_INCLUDE_DIR})
  target_link_libraries(sonora_test_vst3 PRIVATE Threads::Threads)
  set_target_properties(sonora_test_vst3 PROPERTIES
    PREFIX ""
    OUTPUT_NAME sonora-test
    LIBRARY_OUTPUT_DIRECTORY ${SONORA_TEST_VST3}/Contents/${_vst3_arch}
    CXX_VISIBILITY_PRESET hidden)
  add_dependencies(plugin_driver sonora_test_vst3)

  set(_again "vst3:84E8DE5F92554F5396FAE4133C935A18")
  set(_lsp_limiter "vst3:6473702062316D20202020206A7A357A")
  set(_vst3_crash "vst3:534F4E4F524156535433544553540001")
  set(_vst3_drive ${_drive} --bundle ${SONORA_TEST_VST3})
  set(_host $<TARGET_FILE:sonora-plugin-host>)

  add_test(NAME plugin_host_vst3_list
           COMMAND ${_host} --list --json --format vst3 --bundle ${SONORA_TEST_VST3})
  set_tests_properties(plugin_host_vst3_list PROPERTIES PASS_REGULAR_EXPRESSION
    "\"uri\":\"${_vst3_crash}\",\"format\":\"vst3\",\"name\":\"Sonora Test Crash\"")
  add_test(NAME plugin_host_vst3_list_latency
           COMMAND ${_host} --list --json --format vst3)
  set_tests_properties(plugin_host_vst3_list_latency PROPERTIES PASS_REGULAR_EXPRESSION
    "\"uri\":\"${_lsp_limiter}\",\"format\":\"vst3\",\"name\":\"Limiter Mono\",[^\n]*\"latency_frames\":240,")
  # A bundle whose factory segfaults costs its own entry; the scan goes on
  # to the next bundle (AGain, from /usr/lib/vst3).
  add_test(NAME plugin_host_vst3_list_crash_entry
           COMMAND ${_host} --list --json --format vst3 --bundle ${SONORA_TEST_VST3})
  set_tests_properties(plugin_host_vst3_list_crash_entry PROPERTIES
    ENVIRONMENT SONORA_TEST_VST3_CRASH_ON_SCAN=1
    PASS_REGULAR_EXPRESSION "\"bundle\":\"[^\"]*sonora-test.vst3\",\"format\":\"vst3\",\"usable\":false,\"reason\":\"the scan crashed \\(signal 11")
  add_test(NAME plugin_host_vst3_list_after_crash
           COMMAND ${_host} --list --json --format vst3 --bundle ${SONORA_TEST_VST3})
  set_tests_properties(plugin_host_vst3_list_after_crash PROPERTIES
    ENVIRONMENT SONORA_TEST_VST3_CRASH_ON_SCAN=1
    PASS_REGULAR_EXPRESSION "\"uri\":\"${_again}\",\"format\":\"vst3\",\"name\":\"AGain VST3\"")
  add_test(NAME plugin_host_vst3_probe_mismatch
           COMMAND ${_host} --probe "vst3:534F4E4F524156535433544553540002" --channels 1
                   --bundle ${SONORA_TEST_VST3})
  set_tests_properties(plugin_host_vst3_probe_mismatch PROPERTIES PASS_REGULAR_EXPRESSION
    "\"ok\":false,\"reason\":\"[^\"]*did not take the arrangement asked for")
  # A mono-only VST3 on a stereo chain refuses stereo and runs once per channel.
  add_test(NAME plugin_host_vst3_probe_replicated
           COMMAND ${_host} --probe ${_lsp_limiter} --channels 2)
  set_tests_properties(plugin_host_vst3_probe_replicated PROPERTIES PASS_REGULAR_EXPRESSION
    "\"ok\":true,\"channels\":2,\"replicas\":2,\"latency_frames\":240,")
  set_tests_properties(plugin_host_vst3_list plugin_host_vst3_list_latency
    plugin_host_vst3_list_crash_entry plugin_host_vst3_list_after_crash
    plugin_host_vst3_probe_mismatch plugin_host_vst3_probe_replicated PROPERTIES LABELS plugins)

  # --- yabridge (ADR-0017), without Wine. Two bundles laid out as
  # yabridgectl lays them out: Contents/x86_64-linux/<name>.so, a Windows
  # module under Contents/x86_64-win, and the Windows original's
  # moduleinfo.json under Contents/Resources with its class IDs in COM byte
  # order (yabridgectl rewrites only Contents/moduleinfo.json).
  #   sonora-test-bridged.vst3        the test plugin built with the name
  #                                   yabridge's chainloader carries: bridged
  #   sonora-test-multiplatform.vst3  the plain test plugin beside a Windows
  #                                   build, the SDK's multi-platform bundle:
  #                                   native, not bridged
  # Both run the test plugin natively; the real bridge is
  # tools/test-yabridge.sh.
  set(_bridged ${CMAKE_BINARY_DIR}/vst3/sonora-test-bridged.vst3)
  set(_multi ${CMAKE_BINARY_DIR}/vst3/sonora-test-multiplatform.vst3)
  set(_com_classes "")
  foreach(k RANGE 1 10)
    if(k LESS 10)
      set(_kk "0${k}")
    else()
      set(_kk "0A")
    endif()
    if(_com_classes)
      string(APPEND _com_classes ",")
    endif()
    string(APPEND _com_classes "{\"CID\":\"4F4E4F534152535654335445535400${_kk}\",\"Category\":\"Audio Module Class\"}")
  endforeach()
  foreach(_b ${_bridged} ${_multi})
    get_filename_component(_stem ${_b} NAME_WE)
    file(MAKE_DIRECTORY ${_b}/Contents/x86_64-win ${_b}/Contents/Resources)
    file(WRITE ${_b}/Contents/x86_64-win/${_stem}.vst3 "a Windows module stands here\n")
    file(WRITE ${_b}/Contents/Resources/moduleinfo.json
      "{\"Name\":\"${_stem}\",\"Version\":\"1\",\"Classes\":[${_com_classes}]}\n")
  endforeach()
  add_library(sonora_test_vst3_bridged MODULE tests/vst3/sonora_test_vst3.cpp)
  target_compile_definitions(sonora_test_vst3_bridged PRIVATE SONORA_TEST_VST3_FAKE_CHAINLOADER)
  target_include_directories(sonora_test_vst3_bridged SYSTEM PRIVATE ${VST3SDK_INCLUDE_DIR})
  target_link_libraries(sonora_test_vst3_bridged PRIVATE Threads::Threads)
  set_target_properties(sonora_test_vst3_bridged PROPERTIES
    PREFIX ""
    OUTPUT_NAME sonora-test-bridged
    LIBRARY_OUTPUT_DIRECTORY ${_bridged}/Contents/${_vst3_arch}
    CXX_VISIBILITY_PRESET hidden)
  add_custom_command(TARGET sonora_test_vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_multi}/Contents/${_vst3_arch}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonora_test_vst3>
            ${_multi}/Contents/${_vst3_arch}/sonora-test-multiplatform.so)
  add_dependencies(plugin_driver sonora_test_vst3_bridged)

  add_test(NAME plugin_host_yabridge_list_bundles
           COMMAND ${_host} --list --json --bundles-only --format vst3 --bundle ${_bridged} --bundle ${_multi})
  set_tests_properties(plugin_host_yabridge_list_bundles PROPERTIES
    PASS_REGULAR_EXPRESSION "\"bundle\":\"[^\"]*/sonora-test-bridged.vst3\",\"format\":\"vst3\",\"bridge\":\"yabridge\"}"
    FAIL_REGULAR_EXPRESSION "sonora-test-multiplatform.vst3\",\"format\":\"vst3\",\"bridge\"")
  add_test(NAME plugin_host_yabridge_list
           COMMAND ${_host} --list --json --format vst3 --bundle ${_bridged} --bundle ${_multi})
  set_tests_properties(plugin_host_yabridge_list PROPERTIES
    PASS_REGULAR_EXPRESSION "\"uri\":\"vst3:534F4E4F524156535433544553540007\",[^\n]*\"bundle\":\"[^\"]*/sonora-test-bridged.vst3\",\"bridge\":\"yabridge\",\"ports\""
    FAIL_REGULAR_EXPRESSION "sonora-test-multiplatform.vst3\",\"bridge\"")
  # Found through a moduleinfo.json that names it only in COM byte order.
  add_test(NAME plugin_host_yabridge_probe_com_order
           COMMAND ${_host} --probe "vst3:534F4E4F524156535433544553540007" --channels 1 --bundle ${_bridged})
  set_tests_properties(plugin_host_yabridge_probe_com_order PROPERTIES PASS_REGULAR_EXPRESSION
    "\"bundle\":\"[^\"]*/sonora-test-bridged.vst3\",\"bridge\":\"yabridge\"[^\n]*\"ok\":true")
  set_tests_properties(plugin_host_yabridge_list_bundles plugin_host_yabridge_list
    plugin_host_yabridge_probe_com_order PROPERTIES LABELS plugins)

  # The real bridge: a Windows build of the test plugin through Wine and
  # yabridge (tools/Dockerfile.yabridge). Skipped (77) where Wine, yabridge
  # or mingw-w64 is missing, which is CI's plugins image today.
  find_program(SONORA_MINGW_CXX NAMES x86_64-w64-mingw32-g++-posix)
  # One class per Windows bundle (the test plugin's SONORA_TEST_VST3_ONLY_KIND):
  # the State pass-through (kind 9), the Liar (kind 4) and the Crash class
  # (kind 1), which segfaults inside Wine mid-chain.
  set(_win_dir "")
  if(SONORA_MINGW_CXX)
    set(_win_dir ${CMAKE_BINARY_DIR}/vst3-win)
    set(_win_dlls "")
    foreach(_pair "state;9" "liar;4" "crash;1")
      list(GET _pair 0 _name)
      list(GET _pair 1 _kind)
      set(_dll ${_win_dir}/sonora-test-${_name}.vst3/Contents/x86_64-win/sonora-test-${_name}.vst3)
      get_filename_component(_dll_dir ${_dll} DIRECTORY)
      add_custom_command(OUTPUT ${_dll}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dll_dir}
        COMMAND ${SONORA_MINGW_CXX} -std=c++17 -O2 -shared -static -DSONORA_TEST_VST3_ONLY_KIND=${_kind}
                -isystem ${VST3SDK_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/vst3/sonora_test_vst3.cpp
                -o ${_dll}
        DEPENDS tests/vst3/sonora_test_vst3.cpp
        COMMENT "Building the Windows test VST3 sonora-test-${_name} (mingw-w64)")
      list(APPEND _win_dlls ${_dll})
    endforeach()
    add_custom_target(sonora_test_vst3_win ALL DEPENDS ${_win_dlls})
  endif()
  add_test(NAME plugin_host_yabridge_wine
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/yabridge_wine.sh $<TARGET_FILE:sonora-plugin-host>
                   $<TARGET_FILE:plugin_driver> ${SONORA_TEST_VST3})
  set_tests_properties(plugin_host_yabridge_wine PROPERTIES SKIP_RETURN_CODE 77 LABELS plugins TIMEOUT 900
    ENVIRONMENT "SONORA_YABRIDGE_TEST_VST3_DIR=${_win_dir}")

  add_test(NAME plugin_host_vst3_refusals COMMAND ${_vst3_drive} --scenario refusals --format vst3)
  add_test(NAME plugin_host_vst3_gain_mono COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1)
  add_test(NAME plugin_host_vst3_gain_stereo COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1 --channels 2 --expect-replicas 1)
  add_test(NAME plugin_host_vst3_gain_futex COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1 --wake futex)
  add_test(NAME plugin_host_vst3_latency COMMAND ${_vst3_drive} --scenario latency
           --chain ${_lsp_limiter})
  add_test(NAME plugin_host_vst3_latency_futex COMMAND ${_vst3_drive} --scenario latency
           --chain ${_lsp_limiter} --wake futex)
  # kBypassParam through the plugin's own kIsBypass parameter (AGain's).
  add_test(NAME plugin_host_vst3_bypass COMMAND ${_vst3_drive} --scenario bypass
           --chain ${_again} --gain-param 0 --gain-linear 1 --expect-bypass plugin)
  # LV2 and VST3 in one chain, one host process. AGain's controller
  # describes its gain's default as 0.5 (-6 dB) while its processor starts
  # at 1.0; the engine seeds what the description says, so the null needs
  # the gain set to 1, as a session would store it.
  add_test(NAME plugin_host_vst3_latency_mixed COMMAND ${_vst3_drive} --scenario latency
           --chain "http://gareus.org/oss/lv2/dpl#mono,${_lsp_limiter},${_again}" --param 2:0=1)
  # The look-ahead moved while running: ParamID 467309611 is "Lookahead"
  # (normalised 0.738 = 5 ms = 240 frames, `--list --format vst3`). The new
  # latency reaches the host through LSP's processor -> controller IMessage
  # and restartComponent(kLatencyChanged).
  add_test(NAME plugin_host_vst3_latency_change COMMAND ${_vst3_drive} --scenario latency_change
           --chain ${_lsp_limiter} --change-param 467309611 --change-value 0.5)
  # The same through the in-tree latency class, which segfaults if the host
  # asks getLatencySamples() on the thread that runs process(): the re-read
  # after kLatencyChanged belongs on the host's main thread.
  add_test(NAME plugin_host_vst3_latency_change_main_thread COMMAND ${_vst3_drive}
           --scenario latency_change --chain "vst3:534F4E4F524156535433544553540007"
           --change-param 0 --change-value 1)
  # More distinct ParamIDs in one block than the 128 a block once carried.
  add_test(NAME plugin_host_vst3_params COMMAND ${_vst3_drive} --scenario params)
  # A scan that hangs after writing a record longer than a pipe buffer.
  add_test(NAME plugin_host_vst3_list_hang COMMAND ${_vst3_drive} --scenario list_json)
  set_tests_properties(plugin_host_vst3_list_hang PROPERTIES ENVIRONMENT SONORA_TEST_VST3_HANG_ON_SCAN=1)
  add_test(NAME plugin_host_vst3_crash COMMAND ${_vst3_drive} --scenario crash
           --chain "${_again},${_vst3_crash}")
  add_test(NAME plugin_host_vst3_crash_futex COMMAND ${_vst3_drive} --scenario crash
           --chain "${_again},${_vst3_crash}" --wake futex)
  # HostControl with VST3: the handshake and key table on AGain, state
  # through getState/setState, touches from a controller's own thread, and
  # LatencyChanged on the socket after kLatencyChanged.
  add_test(NAME plugin_host_vst3_control COMMAND ${_vst3_drive} --scenario control --chain ${_again})
  add_test(NAME plugin_host_vst3_state COMMAND ${_vst3_drive} --scenario state
           --chain "vst3:534F4E4F524156535433544553540009" --expect-state counter)
  add_test(NAME plugin_host_vst3_gesture COMMAND ${_vst3_drive} --scenario gesture)
  add_test(NAME plugin_host_vst3_latency_change_control COMMAND ${_vst3_drive} --scenario latency_change
           --chain ${_lsp_limiter} --change-param 467309611 --change-value 0.5 --control 1)
  list(APPEND _plugin_tests
    plugin_host_vst3_control plugin_host_vst3_state plugin_host_vst3_gesture
    plugin_host_vst3_latency_change_control
    plugin_host_vst3_refusals
    plugin_host_vst3_gain_mono plugin_host_vst3_gain_stereo plugin_host_vst3_gain_futex
    plugin_host_vst3_latency plugin_host_vst3_latency_futex plugin_host_vst3_latency_mixed
    plugin_host_vst3_latency_change plugin_host_vst3_latency_change_main_thread
    plugin_host_vst3_bypass
    plugin_host_vst3_params plugin_host_vst3_list_hang
    plugin_host_vst3_crash plugin_host_vst3_crash_futex)
endif()

set_tests_properties(${_plugin_tests} PROPERTIES LABELS plugins RUN_SERIAL TRUE)
if(SONORA_PLUGIN_TESTS_ALLOW_SKIP)
  set_tests_properties(${_plugin_tests} PROPERTIES SKIP_RETURN_CODE 77)
endif()

# --- Sonora's own bundles (ADR-0011 N1, the checkpoint of §1.2): every
# native processor loaded as a file through this host, LV2 and VST3, and its
# output compared with the N0 reference render of the same input
# (sonora-dsp-measure --host-fixture). The host never links plugins/native;
# it only waits for the bundles to be built.
get_property(_native GLOBAL PROPERTY SONORA_NATIVE_PLUGINS)
if(_native)
  add_dependencies(plugin_driver sonora_native_bundles)
  set(_fixtures ${CMAKE_BINARY_DIR}/native-host-fixtures)
  file(MAKE_DIRECTORY ${_fixtures})
  # The channel count, the key, the moves and the re-activation are in each
  # case's args file (sonora-dsp-measure --host-fixture writes them).
  set(_render $<TARGET_FILE:plugin_driver> --host $<TARGET_FILE:sonora-plugin-host>
              --scenario render)
  set(_native_tests)
  foreach(n ${_native})
    # --preroll 512: Chain::validate runs 4 blocks of 128 frames of silence
    # through the plugin before the first real block (src/chain.cpp).
    # --vst3-tempo 120: the tempo the VST3 host's ProcessContext carries
    # (src/vst3_host.cpp); the LV2 host sends no transport.
    add_test(NAME native_host_fixture_${n}
             COMMAND sonora-dsp-measure --processor ${n} --host-fixture ${_fixtures}
                     --preroll 512 --vst3-tempo 120
                     --golden ${CMAKE_SOURCE_DIR}/plugins/native/${n}/tests/golden)
    set_tests_properties(native_host_fixture_${n} PROPERTIES FIXTURES_SETUP native_fixture_${n}
                         LABELS plugins)
    # The cases host_fixture.cpp writes for this processor: the stereo
    # bundle's golden, bypass and automation renders; the mono bundle's; the
    # key input's, stereo and mono, for a processor that takes one, routed
    # (key) and not (nokey: the external key on and no --sidechain, which
    # must key from the input, not from silence); and the re-activation for
    # a latency-changing value, for the five that have one.
    set(_cases golden-44100 golden-48000 golden-96000 bypass-48000 automation-48000 mono-48000)
    get_property(_sidechain GLOBAL PROPERTY SONORA_NATIVE_${n}_SIDECHAIN)
    if(_sidechain)
      list(APPEND _cases key-48000 key-mono-48000 nokey-48000 nokey-mono-48000)
    endif()
    if(n STREQUAL "eq" OR n STREQUAL "limiter" OR n STREQUAL "tube" OR n STREQUAL "opto" OR n STREQUAL "fet" OR n STREQUAL "tape")
      list(APPEND _cases reconfigure-48000)
    endif()
    foreach(_case ${_cases})
      # Which bundle: the mono one for the mono cases.
      set(_b ${n})
      if(_case MATCHES "mono")
        set(_b ${n}-mono)
      endif()
      get_property(_uri GLOBAL PROPERTY SONORA_NATIVE_${_b}_LV2_URI)
      get_property(_cid GLOBAL PROPERTY SONORA_NATIVE_${_b}_VST3_ID)
      add_test(NAME native_lv2_${n}_${_case}
               COMMAND ${_render} --bundle ${SONORA_NATIVE_LV2_DIR}/sonora-${_b}.lv2 --chain ${_uri}
                       --args-file ${_fixtures}/${n}-${_case}-lv2.args)
      list(APPEND _native_tests native_lv2_${n}_${_case})
      set_tests_properties(native_lv2_${n}_${_case} PROPERTIES FIXTURES_REQUIRED native_fixture_${n})
      if(VST3SDK_INCLUDE_DIR)
        add_test(NAME native_vst3_${n}_${_case}
                 COMMAND ${_render} --bundle ${SONORA_NATIVE_VST3_DIR}/sonora-${_b}.vst3 --chain ${_cid}
                         --args-file ${_fixtures}/${n}-${_case}-vst3.args)
        list(APPEND _native_tests native_vst3_${n}_${_case})
        set_tests_properties(native_vst3_${n}_${_case} PROPERTIES FIXTURES_REQUIRED native_fixture_${n})
      endif()
    endforeach()

    # HostControl round trips with the bundle as a file (contracts/plugin
    # host_control.h): its state saved, written to "Plugin State/", and
    # restored into a new host byte for byte; and, for the two whose
    # latency moves with a value, the golden re-activation render through
    # Load, then Reconfigure while running.
    get_property(_uri GLOBAL PROPERTY SONORA_NATIVE_${n}_LV2_URI)
    get_property(_cid GLOBAL PROPERTY SONORA_NATIVE_${n}_VST3_ID)
    set(_lv2_bundle ${SONORA_NATIVE_LV2_DIR}/sonora-${n}.lv2)
    set(_vst3_bundle ${SONORA_NATIVE_VST3_DIR}/sonora-${n}.vst3)
    set(_ctl $<TARGET_FILE:plugin_driver> --host $<TARGET_FILE:sonora-plugin-host>)
    add_test(NAME native_lv2_${n}_control_state
             COMMAND ${_ctl} --bundle ${_lv2_bundle} --scenario state --chain ${_uri} --channels 2
                     --expect-state same)
    list(APPEND _native_tests native_lv2_${n}_control_state)
    if(VST3SDK_INCLUDE_DIR)
      add_test(NAME native_vst3_${n}_control_state
               COMMAND ${_ctl} --bundle ${_vst3_bundle} --scenario state --chain ${_cid} --channels 2
                       --expect-state same)
      list(APPEND _native_tests native_vst3_${n}_control_state)
    endif()
    if(n STREQUAL "limiter")
      add_test(NAME native_lv2_limiter_control_render
               COMMAND ${_render} --bundle ${_lv2_bundle} --chain ${_uri} --control 1
                       --args-file ${_fixtures}/limiter-reconfigure-48000-lv2.args)
      add_test(NAME native_lv2_limiter_control_reconfigure
               COMMAND ${_ctl} --bundle ${_lv2_bundle} --scenario reconfigure --chain ${_uri} --channels 2
                       --change-param lookahead --change-value 3 --expect-latency 502)
      set_tests_properties(native_lv2_limiter_control_render PROPERTIES FIXTURES_REQUIRED native_fixture_${n})
      list(APPEND _native_tests native_lv2_limiter_control_render native_lv2_limiter_control_reconfigure)
      if(VST3SDK_INCLUDE_DIR)
        add_test(NAME native_vst3_limiter_control_render
                 COMMAND ${_render} --bundle ${_vst3_bundle} --chain ${_cid} --control 1
                         --args-file ${_fixtures}/limiter-reconfigure-48000-vst3.args)
        add_test(NAME native_vst3_limiter_control_reconfigure
                 COMMAND ${_ctl} --bundle ${_vst3_bundle} --scenario reconfigure --chain ${_cid} --channels 2
                         --change-param Look-ahead --change-value 0.5 --expect-latency 502)
        set_tests_properties(native_vst3_limiter_control_render PROPERTIES FIXTURES_REQUIRED native_fixture_${n})
        list(APPEND _native_tests native_vst3_limiter_control_render native_vst3_limiter_control_reconfigure)
      endif()
    endif()
  endforeach()
  set_tests_properties(${_native_tests} PROPERTIES LABELS plugins)
endif()
