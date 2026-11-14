# sonora-graph-bench (ADR-0218): included from engine/core's CMakeLists.txt.
# It lives here, not in engine/plugin_client, because plugin_client -> core
# is a forbidden edge and the tool drives Session.
set(_bench_dir ${CMAKE_CURRENT_LIST_DIR})
add_executable(sonora-graph-bench
  ${_bench_dir}/graph_bench.cpp ${_bench_dir}/bench_session.cpp ${_bench_dir}/bench_host.cpp
  ${_bench_dir}/bench_report.cpp ${_bench_dir}/heap_counter.cpp)
target_link_libraries(sonora-graph-bench PRIVATE sonora_core sonora_plugin_client sonora_platform)

set(_bench_exe -DBENCH=$<TARGET_FILE:sonora-graph-bench>)
set(_bench_fake --fake --quantum 128 --rate 48000)

# Counts only (ADR-0218 §3): exit 0, one RESULT line with zero drops,
# allocations and lag. No --strict-time: a shared runner's timing decides
# nothing here.
add_test(NAME plugin_client_bench_smoke
         COMMAND ${CMAKE_COMMAND} ${_bench_exe} "-DARGS=${_bench_fake};--callbacks;2000" -DEXPECT_EXIT=0
                 "-DEXPECT_REGEX=RESULT callbacks=2000 p50_us=[0-9.]+ p99_us=[0-9.]+ max_us=[0-9.]+ budget_us=2666.7 misses=0 host_miss=0 drops=0 allocs=0 lag=0 align_err=0 comp=2048 verdict=PASS reasons=none"
                 -P ${_bench_dir}/check_run.cmake)
# The verdict bites: a callback the fake holds past twice the budget is a
# miss, and --strict-time fails the run naming it. Deterministic: that
# callback cannot be under budget, whatever the machine.
add_test(NAME plugin_client_bench_strict_overrun
         COMMAND ${CMAKE_COMMAND} ${_bench_exe} "-DARGS=${_bench_fake};--callbacks;2000;--strict-time;--inject-overrun;100"
                 -DEXPECT_EXIT=1
                 "-DEXPECT_REGEX=RESULT .* misses=[1-9][0-9]* .* drops=0 allocs=0 lag=0 .*verdict=FAIL reasons=[a-z_,]*misses"
                 -P ${_bench_dir}/check_run.cmake)
# Without --strict-time the same overrun is reported and does not fail.
add_test(NAME plugin_client_bench_overrun_reported
         COMMAND ${CMAKE_COMMAND} ${_bench_exe} "-DARGS=${_bench_fake};--callbacks;2000;--inject-overrun;100" -DEXPECT_EXIT=0
                 "-DEXPECT_REGEX=RESULT .* misses=[1-9][0-9]* .*verdict=PASS"
                 -P ${_bench_dir}/check_run.cmake)
set_tests_properties(plugin_client_bench_smoke plugin_client_bench_strict_overrun
                     plugin_client_bench_overrun_reported PROPERTIES LABELS core)

# The real hosts: every chain a sonora-plugin-host, paced at the period. A
# time run, so `timing` and serial; --strict-time is the human's call (the
# PR's laptop line), not this test's.
if(TARGET sonora-plugin-host)
  set(_bench_chain)
  set(_bench_bundles)
  foreach(_p eq-mono compressor-mono delay-mono reverb-mono)
    get_property(_uri GLOBAL PROPERTY SONORA_NATIVE_${_p}_LV2_URI)
    if(_uri)
      list(APPEND _bench_chain --chain ${_uri})
      list(APPEND _bench_bundles --bundle ${SONORA_NATIVE_LV2_DIR}/sonora-${_p}.lv2)
    endif()
  endforeach()
  list(LENGTH _bench_chain _bench_n)
  if(_bench_n EQUAL 8)
    add_dependencies(sonora-graph-bench sonora-plugin-host sonora_native_bundles)
    add_test(NAME plugin_client_bench_real
             COMMAND $<TARGET_FILE:sonora-graph-bench> --host $<TARGET_FILE:sonora-plugin-host>
                     ${_bench_bundles} ${_bench_chain} --callbacks 20000)
    # 77: a Debian plugin of the aligned chain is not installed.
    set_tests_properties(plugin_client_bench_real PROPERTIES LABELS timing RUN_SERIAL TRUE
                         SKIP_RETURN_CODE 77 TIMEOUT 300)
  else()
    message(STATUS "engine/core: no mono native EQ/compressor/delay/reverb; plugin_client_bench_real is not added")
  endif()
endif()
