# ADR-0012 Consequences, auditor A3: common/platform contributes exactly
# wake_all() to the audio thread. Run by ctest (plugin_client_rt_reachability)
# as `cmake -P`, with:
#
#   NM                the toolchain's nm
#   RT_OBJECTS        the object files of engine/plugin_client's audio-thread
#                     half (src/rt.cpp alone, the OBJECT library
#                     sonora_plugin_client_rt)
#   CORE_LIB          engine/core's archive (Session::process)
#   PLATFORM_INCLUDE  common/platform/include
#
# Three checks, each failing the test:
#  1. The audio thread's object references no common/platform symbol but
#     sonora::platform::wake_all, and nothing that allocates, frees, locks,
#     blocks, sleeps, logs, writes or throws. A symbol it does not reference
#     cannot be reached from it; what it calls inline is compiled into it and
#     shows up here as its own undefined references.
#  2. engine/core references no common/platform symbol at all: when
#     Session::process gains its InsertChain step it reaches the platform
#     only through check 1's object. Revisit this check if engine/core ever
#     links common/platform for its non-RT threads; the rule stays "nothing
#     but wake_all from Session::process".
#  3. wake_all is the only SONORA_RT declaration in common/platform's
#     headers.

cmake_minimum_required(VERSION 3.16)

set(failures 0)

function(undefined_symbols file out)
  execute_process(COMMAND ${NM} -C -u ${file}
                  OUTPUT_VARIABLE text ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${NM} -C -u ${file} failed (${rc}): ${err}")
  endif()
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

# Demangled names (nm -C). Sanitizer and stack-protector hooks are not on
# the list: they are the build's instrumentation, not the code's calls.
set(denied
  "operator new" "operator delete"
  "^malloc$" "^calloc$" "^realloc$" "^free$" "posix_memalign" "aligned_alloc"
  "pthread_" "^sem_" "mtx_" "cnd_"
  "__cxa_throw" "__cxa_allocate_exception" "__cxa_guard_acquire" "std::__throw_"
  "printf" "^puts$" "^fwrite$" "^fputs$" "^write$" "^writev$" "syslog"
  "sleep" "^syscall$" "futex" "^poll$" "^select$" "^epoll" "^open" "^read$" "^close$"
  "^mmap" "^munmap$" "^mlock")

list(LENGTH RT_OBJECTS n_objects)
if(n_objects EQUAL 0)
  message(FATAL_ERROR "RT_OBJECTS is empty")
endif()
foreach(obj IN LISTS RT_OBJECTS)
  undefined_symbols(${obj} symbols)
  list(LENGTH symbols n)
  message(STATUS "${obj}: ${n} undefined symbol(s)")
  foreach(sym IN LISTS symbols)
    message(STATUS "  ${sym}")
    if(sym MATCHES "sonora::platform::" AND NOT sym MATCHES "^sonora::platform::wake_all\\(")
      message(SEND_ERROR "audio thread reaches common/platform beyond wake_all: ${sym}")
      math(EXPR failures "${failures} + 1")
    endif()
    foreach(pattern IN LISTS denied)
      if(sym MATCHES "${pattern}")
        message(SEND_ERROR "audio thread references '${sym}' (matches ${pattern})")
        math(EXPR failures "${failures} + 1")
      endif()
    endforeach()
  endforeach()
endforeach()

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
      message(STATUS "SONORA_RT in common/platform: ${line}")
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
message(STATUS "rt reachability: ok (wake_all is the only common/platform symbol on the audio thread's path)")
