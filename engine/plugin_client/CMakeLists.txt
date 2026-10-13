# engine/plugin_client (ADR-0012 checklist 4): the engine's side of the
# plugin boundary, on contracts/plugin layout 1. Not linked by engine/core
# yet: the InsertChain plan node (checklist 3) and the control server's
# wiring (checklist 5) come after PR #46.
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
target_link_libraries(sonora_plugin_client_rt PUBLIC sonora_plugin_protocol sonora_platform sonora_base)

add_library(sonora_plugin_client STATIC src/client.cpp src/host_supervisor.cpp
            include/sonora/plugin_client/host_supervisor.h $<TARGET_OBJECTS:sonora_plugin_client_rt>)
target_include_directories(sonora_plugin_client PUBLIC include)
target_link_libraries(sonora_plugin_client PUBLIC sonora_plugin_protocol sonora_platform sonora_base
                      Threads::Threads)

# The audio thread's half against a scripted in-process host: no process,
# no plugin, any OS.
add_executable(test_plugin_client tests/test_plugin_client.cpp)
target_link_libraries(test_plugin_client PRIVATE sonora_plugin_client)
add_test(NAME plugin_client COMMAND test_plugin_client)

# A3: nothing in common/platform but wake_all is reachable from the audio
# thread, checked on the objects with nm, BEFORE any SONORA_RT caller of
# wake_all lands (ADR-0012 Consequences).
if(CMAKE_NM AND NOT MSVC)
  add_test(NAME plugin_client_rt_reachability
           COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                   "-DRT_OBJECTS=$<TARGET_OBJECTS:sonora_plugin_client_rt>"
                   -DCORE_LIB=$<TARGET_FILE:sonora_core>
                   -DPLATFORM_INCLUDE=${CMAKE_SOURCE_DIR}/common/platform/include
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rt_reachability.cmake)
  # The check bites: an object that allocates, locks and calls another
  # common/platform function must fail it.
  add_library(plugin_client_rt_canary OBJECT tests/rt_reachability_canary.cpp)
  target_link_libraries(plugin_client_rt_canary PRIVATE sonora_platform)
  add_test(NAME plugin_client_rt_reachability_canary
           COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
                   "-DRT_OBJECTS=$<TARGET_OBJECTS:plugin_client_rt_canary>"
                   -DCORE_LIB=$<TARGET_FILE:sonora_core>
                   -DPLATFORM_INCLUDE=${CMAKE_SOURCE_DIR}/common/platform/include
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rt_reachability.cmake)
  set_tests_properties(plugin_client_rt_reachability_canary PROPERTIES PASS_REGULAR_EXPRESSION
    "beyond wake_all:[ \n]+sonora::platform::executable_dir")
else()
  message(STATUS "engine/plugin_client: no nm; the A3 reachability test is skipped")
endif()

# ADR-0012 Unverified 10: RealtimeSanitizer on wake_all() from a
# nonblocking function. Needs Clang 20 or later; the engine's toolchain is
# GCC 14, so this is skipped there with one status line, and the RTSan CI
# job is BACKLOG Track 4 item 4.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=realtime)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=realtime)
check_cxx_source_compiles("void f() noexcept [[clang::nonblocking]] {} int main() { f(); }"
                          SONORA_HAVE_RTSAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(SONORA_HAVE_RTSAN)
  add_executable(test_rtsan_wake tests/test_rtsan_wake.cpp)
  target_compile_options(test_rtsan_wake PRIVATE -fsanitize=realtime)
  target_link_options(test_rtsan_wake PRIVATE -fsanitize=realtime)
  target_link_libraries(test_rtsan_wake PRIVATE sonora_platform)
  add_test(NAME plugin_client_rtsan_wake COMMAND test_rtsan_wake)
  set_tests_properties(plugin_client_rtsan_wake PROPERTIES ENVIRONMENT RTSAN_OPTIONS=halt_on_error=false)
else()
  message(STATUS "engine/plugin_client: no -fsanitize=realtime in this compiler; the RTSan check of "
                 "wake_all (ADR-0012 Unverified 10) is skipped")
endif()

# End to end: drivers/fake drives the callback, the supervisor starts the
# real sonora-plugin-host (built only where lilv is, tools/build-plugins.sh
# and CI's plugins job), with the in-tree test plugins and Sonora's own EQ
# bundle against its N0 reference render.
if(TARGET sonora-plugin-host)
  add_executable(test_plugin_client_host tests/test_plugin_client_host.cpp)
  target_link_libraries(test_plugin_client_host PRIVATE sonora_plugin_client sonora_backend_fake)
  add_dependencies(test_plugin_client_host sonora-plugin-host sonora_test_lv2)
  set(_pc_host $<TARGET_FILE:test_plugin_client_host> --host $<TARGET_FILE:sonora-plugin-host>
               --bundle ${CMAKE_BINARY_DIR}/lv2/sonora-test.lv2)
  set(_pc_tests)
  foreach(scenario passthrough params miss crash restarts nan)
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
