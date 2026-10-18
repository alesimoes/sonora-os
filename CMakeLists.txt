cmake_minimum_required(VERSION 3.16)
project(sonora LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# The RealtimeSanitizer build (tools/build-rtsan.sh, CI's rtsan job; ADR-0007,
# first 2026-10-12 update). Every SONORA_RT function is [[clang::nonblocking]]
# under this compiler, so RTSan aborts a test that allocates, locks or makes
# a blocking call inside one. -Wfunction-effects is the same rule read at
# compile time; it stays a warning because it cannot see through libstdc++
# (it infers nothing past std::__throw_length_error), and the runtime check
# is the gate.
option(SONORA_RTSAN "Build with -fsanitize=realtime (Clang 20 or later)" OFF)
if(SONORA_RTSAN)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
    message(FATAL_ERROR "SONORA_RTSAN needs Clang 20 or later; this is ${CMAKE_CXX_COMPILER_ID} "
                        "${CMAKE_CXX_COMPILER_VERSION}")
  endif()
  add_compile_options(-fsanitize=realtime -fno-omit-frame-pointer -Wfunction-effects)
  add_link_options(-fsanitize=realtime)
endif()

# Any sanitizer build: SONORA_RTSAN, or -fsanitize= (/fsanitize= on MSVC) in
# the flags, as CI's TSan and ASan/UBSan jobs and the nightly asan build pass
# them. A sanitizer slows code by a factor nobody knows, so the native
# suites' wall-clock and CPU bars are reported there, not held (the measure
# harness's Suite::check_time_le); every correctness bar is held. A CMake
# define rather than the compiler's macros alone: GCC announces UBSan with
# none.
string(TOUPPER "${CMAKE_BUILD_TYPE}" SONORA_BUILD_TYPE_UPPER)
if(SONORA_RTSAN OR "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${SONORA_BUILD_TYPE_UPPER}}" MATCHES "(^| )[-/]fsanitize=")
  set(SONORA_SANITIZED ON)
else()
  set(SONORA_SANITIZED OFF)
endif()
message(STATUS "Sanitizer build (time bars reported, not held): ${SONORA_SANITIZED}")

# Every executable lands in the build root, so build/sonora-engine stays where
# tools/build-iso.sh and tools/test-engine-docker.sh look for it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Threads REQUIRED)

# Before any add_subdirectory(), so the root CTestTestfile picks up every
# module's tests and `ctest --test-dir build` runs them all.
enable_testing()

# Modules and the dependency graph: ADR-0007 §1. Each module exposes only its
# own include/ directory, so an include across an edge it does not link fails
# to compile. Listed by layer; a module may only link the ones above it.
add_subdirectory(common/base)
# Strict JSON for the session file and the key maps (ADR-0009 §5), layer 0.
add_subdirectory(common/json)
add_subdirectory(common/audiofile)
# The session document (ADR-0009), layer 1 beside common/audiofile.
add_subdirectory(common/model)
add_subdirectory(common/midi)
# The OS seam (ADR-0008). Linked only by modules that need an OS service.
add_subdirectory(common/platform)
# Neutral DSP for the native plugin suite and the meters (ADR-0011 §2.1),
# layer 1 on common/base.
add_subdirectory(common/dsp)
# The native plugin suite (ADR-0011): the Processor interface, the format
# adapters (each processor's directory asks them for its LV2 and VST3
# bundles), then the measurement harness, then each processor.
add_subdirectory(plugins/native/sdk)
add_subdirectory(plugins/native/format)
add_subdirectory(plugins/native/measure)
add_subdirectory(plugins/native/eq)
add_subdirectory(plugins/native/compressor)
add_subdirectory(plugins/native/gate)
add_subdirectory(plugins/native/limiter)
add_subdirectory(plugins/native/delay)
add_subdirectory(plugins/native/chorus)
add_subdirectory(plugins/native/reverb)
add_subdirectory(plugins/native/tube)
add_subdirectory(plugins/native/opto)
add_subdirectory(plugins/native/measure/app)
add_subdirectory(contracts/api)
add_subdirectory(contracts/backend)
# The engine <-> plugin host shared-memory layout, accepted as layout 1
# (ADR-0012 §5).
add_subdirectory(contracts/plugin)
# Optional: skipped, with one status line, where lilv is not installed.
# tools/build-plugins.sh builds and tests it. Before engine/plugin_client,
# whose end-to-end tests run it when it is built.
add_subdirectory(plugins/host)
add_subdirectory(engine/core)
add_subdirectory(drivers/support)
add_subdirectory(drivers/jack)
# Platform-neutral (ADR-0008): the backend engine tests run on, any OS.
add_subdirectory(drivers/fake)
# The engine's side of the plugin boundary (ADR-0012 checklist 4). Linked
# by engine/core, added above, for PluginChainRunner; it links
# common/platform PRIVATE so engine/core still cannot include a platform
# header (ADR-0007 §1).
add_subdirectory(engine/plugin_client)
add_subdirectory(ui/keymap)
add_subdirectory(ui/tui)
add_subdirectory(engine/control)
add_subdirectory(engine/app)
# The interfaces' side of the socket (M7). After engine/app: its test runs
# sonora-engine as a process, a build-order dependency, never a link.
add_subdirectory(ui/client)
# Optional: skipped, with one status line, where Qt 6 is not installed (the
# engine's build container and CI). tools/build-ui.sh builds it.
add_subdirectory(ui/qt)
