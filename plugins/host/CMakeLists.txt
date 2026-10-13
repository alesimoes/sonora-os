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
# test driver. The engine's half moves to common/platform with M4+ (ipc.h).
# start_thread: the host's processing thread.
find_package(Threads REQUIRED)
add_library(sonora_plugin_ipc STATIC src/ipc.h src/linux/ipc_linux.cpp)
target_include_directories(sonora_plugin_ipc PUBLIC src)
target_link_libraries(sonora_plugin_ipc PUBLIC sonora_plugin_protocol Threads::Threads)

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
               src/lv2_host.cpp src/lv2_host.h)
target_link_libraries(sonora-plugin-host PRIVATE sonora_plugin_ipc sonora_plugin_loader
                      sonora_urid_map PkgConfig::LILV)
if(VST3SDK_INCLUDE_DIR)
  target_sources(sonora-plugin-host PRIVATE src/vst3_host.cpp src/vst3_host.h)
  target_include_directories(sonora-plugin-host SYSTEM PRIVATE ${VST3SDK_INCLUDE_DIR})
  target_compile_definitions(sonora-plugin-host PRIVATE SONORA_HAVE_VST3)
endif()

# --- tests (ADR-0007 §7: a fake engine driver plus in-tree test plugins)

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

  add_test(NAME plugin_host_vst3_refusals COMMAND ${_vst3_drive} --scenario refusals --format vst3)
  add_test(NAME plugin_host_vst3_gain_mono COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1)
  add_test(NAME plugin_host_vst3_gain_stereo COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1 --channels 2 --expect-replicas 1)
  add_test(NAME plugin_host_vst3_gain_poll COMMAND ${_vst3_drive} --scenario gain
           --chain ${_again} --gain-param 0 --gain-linear 1 --mode poll)
  add_test(NAME plugin_host_vst3_latency COMMAND ${_vst3_drive} --scenario latency
           --chain ${_lsp_limiter})
  add_test(NAME plugin_host_vst3_latency_poll COMMAND ${_vst3_drive} --scenario latency
           --chain ${_lsp_limiter} --mode poll)
  # LV2 and VST3 in one chain, one host process.
  add_test(NAME plugin_host_vst3_latency_mixed COMMAND ${_vst3_drive} --scenario latency
           --chain "http://gareus.org/oss/lv2/dpl#mono,${_lsp_limiter},${_again}")
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
  add_test(NAME plugin_host_vst3_crash_poll COMMAND ${_vst3_drive} --scenario crash
           --chain "${_again},${_vst3_crash}" --mode poll)
  list(APPEND _plugin_tests
    plugin_host_vst3_refusals
    plugin_host_vst3_gain_mono plugin_host_vst3_gain_stereo plugin_host_vst3_gain_poll
    plugin_host_vst3_latency plugin_host_vst3_latency_poll plugin_host_vst3_latency_mixed
    plugin_host_vst3_latency_change plugin_host_vst3_latency_change_main_thread
    plugin_host_vst3_params plugin_host_vst3_list_hang
    plugin_host_vst3_crash plugin_host_vst3_crash_poll)
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
    # key input's, stereo and mono, for a processor that takes one; and the
    # re-activation for a latency-changing value, for the two that have one.
    set(_cases golden-44100 golden-48000 golden-96000 bypass-48000 automation-48000 mono-48000)
    get_property(_sidechain GLOBAL PROPERTY SONORA_NATIVE_${n}_SIDECHAIN)
    if(_sidechain)
      list(APPEND _cases key-48000 key-mono-48000)
    endif()
    if(n STREQUAL "eq" OR n STREQUAL "limiter")
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
  endforeach()
  set_tests_properties(${_native_tests} PROPERTIES LABELS plugins)
endif()
