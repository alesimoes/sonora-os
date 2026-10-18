# engine/plugin_client (ADR-0012 checklist 4): the engine's side of the
# plugin boundary, on contracts/plugin layout 1. engine/core links it for
# PluginChainRunner (engine/core/include/sonora/plugin_chain_runner.h), the
# InsertChain node's adapter; the control server's wiring is checklist 5.
#
# Two libraries, so the audio thread's half can be checked on its own:
#   sonora_plugin_client_rt  src/rt.cpp only: what Session::process will
#                            call. tests/rt_reachability.cmake reads its
#                            object for anything an audio thread must not
#                            reach (auditor A3).
#   sonora_plugin_client     the tables' control-thread half and the host
#                            supervisor (never on the audio thread).
add_library(sonora_plugin_client_rt OBJECT src/rt.cpp src/tables.h
            include/sonora/plugin_client/plugin_client.h)
target_include_directories(sonora_plugin_client_rt PUBLIC include)
# common/platform for wake_all() alone, behind kHostWakeEnabled (A3).
# PRIVATE here and below: no public header of plugin_client includes a
# platform header, and engine/core links plugin_client, so a PUBLIC edge
# would hand engine/core common/platform's include directory and open the
# compile-time wall ADR-0007 §1 relies on. A user of plugin_client that
# needs common/platform itself (its tests, engine/control) links it.
target_link_libraries(sonora_plugin_client_rt PUBLIC sonora_plugin_protocol sonora_base
                      PRIVATE sonora_platform)

add_library(sonora_plugin_client STATIC src/client.cpp src/host_supervisor.cpp
            include/sonora/plugin_client/host_supervisor.h $<TARGET_OBJECTS:sonora_plugin_client_rt>)
target_include_directories(sonora_plugin_client PUBLIC include)
# contracts/plugin's HostControl codec (host_control.h): the supervisor
# speaks it to every host, and host_supervisor.h names its types.
target_link_libraries(sonora_plugin_client PUBLIC sonora_plugin_protocol sonora_plugin_host_control sonora_base
                      Threads::Threads PRIVATE sonora_platform)

# The audio thread's half against a scripted in-process host: no process,
# no plugin, any OS.
add_executable(test_plugin_client tests/test_plugin_client.cpp)
target_link_libraries(test_plugin_client PRIVATE sonora_plugin_client sonora_platform)
add_test(NAME plugin_client COMMAND test_plugin_client)

# A3: nothing in common/platform but wake_all is reachable from the audio
# thread, checked on the objects with nm, BEFORE any SONORA_RT caller of
# wake_all lands (ADR-0012 Consequences).
if(CMAKE_NM AND NOT MSVC)
  function(sonora_rt_reachability name objects)
    add_test(NAME ${name}
             COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                     "-DRT_OBJECTS=${objects}"
                     "-DWAKE_OBJECTS=$<TARGET_OBJECTS:sonora_platform>"
                     -DCORE_LIB=$<TARGET_FILE:sonora_core>
                     -DPLATFORM_INCLUDE=${CMAKE_SOURCE_DIR}/common/platform/include
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rt_reachability.cmake)
  endfunction()
  # engine/core's PluginChainRunner is on the audio thread too (auditor
  # finding 3 on PR #59): its object is held to the same allowlist.
  sonora_rt_reachability(plugin_client_rt_reachability
    "$<TARGET_OBJECTS:sonora_plugin_client_rt>;$<TARGET_OBJECTS:sonora_core_chain_runner>")
  # The check bites, one denial per canary. Each canary object must fail it
  # for its own reason; the test passes only on that reason's message.
  add_library(plugin_client_rt_canary_platform OBJECT tests/canary_platform.cpp)
  target_link_libraries(plugin_client_rt_canary_platform PRIVATE sonora_platform)
  add_library(plugin_client_rt_canary_alloc OBJECT tests/canary_alloc.cpp)
  # The string canary at -O2 whatever the build type: the case where
  # libstdc++'s extern template keeps the allocation out of the object.
  add_library(plugin_client_rt_canary_string OBJECT tests/canary_string.cpp)
  target_compile_options(plugin_client_rt_canary_string PRIVATE -O2)
  sonora_rt_reachability(plugin_client_rt_canary_platform "$<TARGET_OBJECTS:plugin_client_rt_canary_platform>")
  set_tests_properties(plugin_client_rt_canary_platform PROPERTIES PASS_REGULAR_EXPRESSION
    "beyond wake_all:[ \n]+sonora::platform::current_process_id")
  sonora_rt_reachability(plugin_client_rt_canary_alloc "$<TARGET_OBJECTS:plugin_client_rt_canary_alloc>")
  set_tests_properties(plugin_client_rt_canary_alloc PROPERTIES PASS_REGULAR_EXPRESSION
    "references 'operator new")
  sonora_rt_reachability(plugin_client_rt_canary_string "$<TARGET_OBJECTS:plugin_client_rt_canary_string>")
  set_tests_properties(plugin_client_rt_canary_string PROPERTIES PASS_REGULAR_EXPRESSION
    "which is not on[ \n]+its allowlist")
else()
  message(STATUS "engine/plugin_client: no nm; the A3 reachability test is skipped")
endif()

# ADR-0012 Unverified 10: RealtimeSanitizer on wake_all() from a
# nonblocking function. Only in the SONORA_RTSAN build (tools/build-rtsan.sh,
# CI's rtsan job): wake_all's scope exists only when common/platform itself
# is compiled with the sanitizer. Two tests: wake_all is clean under
# halt_on_error=true, and the same syscall outside that scope is reported,
# so the clean run means the scope works and not that RTSan missed it.
if(SONORA_RTSAN)
  add_executable(test_rtsan_wake tests/test_rtsan_wake.cpp)
  target_link_libraries(test_rtsan_wake PRIVATE sonora_platform)
  add_test(NAME plugin_client_rtsan_wake COMMAND test_rtsan_wake)
  set_tests_properties(plugin_client_rtsan_wake PROPERTIES ENVIRONMENT RTSAN_OPTIONS=halt_on_error=true)
  add_test(NAME plugin_client_rtsan_raw_syscall COMMAND test_rtsan_wake --raw-syscall)
  set_tests_properties(plugin_client_rtsan_raw_syscall PROPERTIES
    ENVIRONMENT RTSAN_OPTIONS=halt_on_error=true
    PASS_REGULAR_EXPRESSION "real-time unsafe function `syscall`"
    LABELS rtsan-report)
  # The scope closes: a syscall after wake_all in the same context is reported.
  add_test(NAME plugin_client_rtsan_after_wake COMMAND test_rtsan_wake --after-wake)
  set_tests_properties(plugin_client_rtsan_after_wake PROPERTIES
    ENVIRONMENT RTSAN_OPTIONS=halt_on_error=true
    PASS_REGULAR_EXPRESSION "real-time unsafe function `syscall`.*raw_wake"
    LABELS rtsan-report)
else()
  message(STATUS "engine/plugin_client: not a SONORA_RTSAN build; the RTSan check of "
                 "wake_all (ADR-0012 Unverified 10) runs in tools/build-rtsan.sh")
endif()

# End to end: drivers/fake drives the callback, the supervisor starts the
# real sonora-plugin-host (built only where lilv is, tools/build-plugins.sh
# and CI's plugins job), with the in-tree test plugins and Sonora's own EQ
# bundle against its N0 reference render.
if(TARGET sonora-plugin-host)
  add_executable(test_plugin_client_host tests/test_plugin_client_host.cpp)
  target_link_libraries(test_plugin_client_host PRIVATE sonora_plugin_client sonora_backend_fake sonora_platform)
  add_dependencies(test_plugin_client_host sonora-plugin-host sonora_test_lv2)
  set(_pc_host $<TARGET_FILE:test_plugin_client_host> --host $<TARGET_FILE:sonora-plugin-host>
               --bundle ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
  set(_pc_tests)
  foreach(scenario control_loaded control_refused_instance control_bad_hello control_bogus control_flood
                   control_half async_start ready_timeout async_refused passthrough params miss crash restarts nan)
    add_test(NAME plugin_client_host_${scenario} COMMAND ${_pc_host} --scenario ${scenario})
    list(APPEND _pc_tests plugin_client_host_${scenario})
  endforeach()
  get_property(_eq_uri GLOBAL PROPERTY SONORA_NATIVE_eq_LV2_URI)
  if(_eq_uri)
    add_dependencies(test_plugin_client_host sonora_native_bundles)
    add_test(NAME plugin_client_host_native_eq
             COMMAND ${_pc_host} --scenario native --bundle ${SONORA_NATIVE_LV2_DIR}/sonora-eq.lv2
                     --chain ${_eq_uri} --args-file ${CMAKE_BINARY_DIR}/native-host-fixtures/eq-golden-48000-lv2.args)
    set_tests_properties(plugin_client_host_native_eq PROPERTIES FIXTURES_REQUIRED native_fixture_eq)
    list(APPEND _pc_tests plugin_client_host_native_eq)
  endif()
  set_tests_properties(${_pc_tests} PROPERTIES LABELS plugins RUN_SERIAL TRUE)
endif()
