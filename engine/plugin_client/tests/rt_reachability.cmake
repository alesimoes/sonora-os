# ADR-0012 Consequences, auditor A3: common/platform contributes exactly
# wake_all() to the audio thread. Run by ctest (plugin_client_rt_reachability
# and its canaries) as `cmake -P`, with:
#
#   NM                the toolchain's nm
#   RT_OBJECTS        the object files of the audio thread's half (src/rt.cpp
#                     alone, the OBJECT library sonora_plugin_client_rt)
#   WAKE_OBJECTS      common/platform's objects; the one built from wake.cpp
#                     is checked as wake_all's body
#   CORE_LIB          engine/core's archive (Session::process)
#   PLATFORM_INCLUDE  common/platform/include
#
# An ALLOWLIST, not a list of known-bad names (rt-safety-auditor M1 on PR
# #49): every undefined symbol of the audio thread's object must be one of
# the few below, so any out-of-line callee at all, whatever its name (a
# libstdc++ string member, a condition variable, clock_gettime), fails. What
# the object calls inline is compiled into it, and its own references show
# up here. One level deeper, wake_all's object may reach only the raw
# syscall. Checks:
#  1. rt.cpp's object: only the allowlist.
#  2. wake.cpp's object: only syscall (and the same instrumentation).
#  3. engine/core references no common/platform symbol: Session::process
#     reaches the platform only through check 1's object. Revisit if
#     engine/core ever links common/platform for its non-RT threads.
#  4. wake_all is the only SONORA_RT declaration in common/platform.

cmake_minimum_required(VERSION 3.16)

set(failures 0)

function(undefined_symbols file out)
  execute_process(COMMAND ${NM} -C -u ${file}
                  OUTPUT_VARIABLE text ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${NM} -C -u ${file} failed (${rc}): ${err}")
  endif()
  # Demangled names can hold ';' and brackets; neither matters here.
  string(REPLACE ";" "," text "${text}")
  string(REPLACE "[" "<" text "${text}")
  string(REPLACE "]" ">" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(symbols)
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line MATCHES "^[Uw] (.+)$")
      list(APPEND symbols "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set(${out} "${symbols}" PARENT_SCOPE)
endfunction()

# The build's own instrumentation (sanitizers, stack protector), never the
# code's calls.
set(instrumentation "^__(asan|tsan|ubsan|msan|lsan|sanitizer)_" "^__stack_chk_fail$" "^__stack_chk_guard$")

function(check_allowed obj what)
  set(allowed ${ARGN})
  undefined_symbols(${obj} symbols)
  list(LENGTH symbols n)
  message(STATUS "${obj}: ${n} undefined symbol(s)")
  foreach(sym IN LISTS symbols)
    message(STATUS "  ${sym}")
    set(ok FALSE)
    foreach(pattern IN LISTS allowed instrumentation)
      if(sym MATCHES "${pattern}")
        set(ok TRUE)
      endif()
    endforeach()
    if(NOT ok)
      if(sym MATCHES "sonora::platform::")
        message(SEND_ERROR "${what} reaches common/platform beyond wake_all: ${sym}")
      else()
        message(SEND_ERROR "${what} references '${sym}', which is not on its allowlist")
      endif()
      math(EXPR failures "${failures} + 1")
      set(failures ${failures} PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

list(LENGTH RT_OBJECTS n_objects)
if(n_objects EQUAL 0)
  message(FATAL_ERROR "RT_OBJECTS is empty")
endif()
foreach(obj IN LISTS RT_OBJECTS)
  check_allowed(${obj} "the audio thread"
                "^memcpy$" "^memset$" "^memmove$" "^sonora::platform::wake_all\\(")
endforeach()

set(found_wake FALSE)
foreach(obj IN LISTS WAKE_OBJECTS)
  get_filename_component(name ${obj} NAME)
  if(name MATCHES "^wake\\.cpp\\.o(bj)?$")
    set(found_wake TRUE)
    check_allowed(${obj} "wake_all's object" "^syscall$")
  endif()
endforeach()
if(NOT found_wake)
  message(SEND_ERROR "common/platform's wake.cpp object was not among WAKE_OBJECTS")
  math(EXPR failures "${failures} + 1")
endif()

undefined_symbols(${CORE_LIB} core_symbols)
foreach(sym IN LISTS core_symbols)
  if(sym MATCHES "sonora::platform::")
    message(SEND_ERROR "engine/core references common/platform: ${sym}")
    math(EXPR failures "${failures} + 1")
  endif()
endforeach()

# Read whole and split on newlines by hand: a declaration's ';' would split
# it again as a CMake list, so the count is kept in a plain counter.
file(GLOB_RECURSE headers ${PLATFORM_INCLUDE}/*.h)
set(n_rt 0)
set(n_wake 0)
foreach(h IN LISTS headers)
  file(READ ${h} text)
  string(REPLACE ";" "," text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line MATCHES "SONORA_RT" AND NOT line MATCHES "^//" AND NOT line MATCHES "^#")
      math(EXPR n_rt "${n_rt} + 1")
      if(line MATCHES "wake_all\\(")
        math(EXPR n_wake "${n_wake} + 1")
      endif()
    endif()
  endforeach()
endforeach()
if(NOT n_rt EQUAL 1 OR NOT n_wake EQUAL 1)
  message(SEND_ERROR "common/platform must declare exactly one SONORA_RT function, wake_all; found ${n_rt}")
  math(EXPR failures "${failures} + 1")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} reachability violation(s)")
endif()
message(STATUS "rt reachability: ok (allowlist held; wake_all is the only common/platform symbol on the audio thread's path)")
