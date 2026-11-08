cmake_minimum_required(VERSION 3.16)
project(sonora LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Per compiler: MSVC does not take -Wall/-Wextra (it reads them as /Wall-like
# or unknown options).
add_compile_options(
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# The platform selection (ADR-0008 §3). linux builds everything the appliance
# ships; neutral builds the modules that are standard C++17 only, which the
# `portable` CI job compiles on Windows with MSVC; windows is reserved.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(SONORA_PLATFORM_DEFAULT linux)
else()
  set(SONORA_PLATFORM_DEFAULT neutral)
endif()
set(SONORA_PLATFORM "${SONORA_PLATFORM_DEFAULT}" CACHE STRING
    "Platform to build for: linux or neutral (windows is reserved)")
set_property(CACHE SONORA_PLATFORM PROPERTY STRINGS linux neutral windows)
if(SONORA_PLATFORM STREQUAL "windows")
  message(FATAL_ERROR "drivers/asio and the Windows entry layer do not exist yet; "
                      "use SONORA_PLATFORM=neutral")
elseif(NOT SONORA_PLATFORM STREQUAL "linux" AND NOT SONORA_PLATFORM STREQUAL "neutral")
  message(FATAL_ERROR "SONORA_PLATFORM must be linux or neutral, not '${SONORA_PLATFORM}'")
endif()
if(SONORA_PLATFORM STREQUAL "linux")
  set(SONORA_BACKENDS_DEFAULT "jack;fake")
else()
  set(SONORA_BACKENDS_DEFAULT "fake")
endif()
set(SONORA_BACKENDS "${SONORA_BACKENDS_DEFAULT}" CACHE STRING
    "Audio backends linked into the entry layer")
message(STATUS "SONORA_PLATFORM=${SONORA_PLATFORM} SONORA_BACKENDS=${SONORA_BACKENDS}")

# The plugin package is a product of its own (owner, 2026-11-05): with this
# on, only what the native plugins and their tests need is configured
# (common/*, plugins/native), not the engine, the host, the drivers or the
# UI. tools/build-plugins.sh --package and tools/package-plugins.sh use it.
# Windows VST3 bundles (MSVC), for the Windows package only; off in the
# Portable job so it stays a test of the neutral modules.
option(SONORA_WINDOWS_VST3 "Build the native plugins as Windows VST3 bundles" OFF)
option(SONORA_PLUGINS_ONLY "Configure only the native plugin suite and what it links" OFF)
# ADR-0071: under SONORA_PLATFORM=neutral, also configure ui/qt and the modules
# it links, so the nightly's Portable job builds the Qt interface under MSVC.
# Off elsewhere; the Linux build already configures all of it.
option(SONORA_PORTABLE_UI "With SONORA_PLATFORM=neutral, also configure ui/qt" OFF)

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
if(SONORA_PLATFORM STREQUAL "linux" OR SONORA_PORTABLE_UI)
add_subdirectory(common/midi)
# The session document (ADR-0009), layer 1 beside common/audiofile. It links
# common/midi for notes and edits (ADR-0014 §1), above it in this list.
add_subdirectory(common/model)
endif()
# The OS seam (ADR-0008). Linked only by modules that need an OS service.
add_subdirectory(common/platform)
# The take file (BWF writer, reader, crash recovery). Standard C++ plus the
# OS seam, so the neutral build and the MSVC job compile it (ADR-0008 P2).
add_subdirectory(common/audiofile)
if((SONORA_PLATFORM STREQUAL "linux" OR SONORA_PORTABLE_UI) AND NOT SONORA_PLUGINS_ONLY)
# A session folder on disk (ADR-0015 §3, §6), layer 2 on common/model and
# common/platform: the engine's saver writes through it.
add_subdirectory(common/session_files)
endif()
# Neutral DSP for the native plugin suite and the meters (ADR-0011 §2.1),
# layer 1 on common/base.
add_subdirectory(common/dsp)
# The native plugin suite (ADR-0011): the SDK, the format adapters, the
# measurement harness, and every processor directory found under
# plugins/native (see plugins/native/README.md: adding one edits no list).
add_subdirectory(plugins/native)
if(SONORA_PLATFORM STREQUAL "linux" AND NOT SONORA_PLUGINS_ONLY)
# sonora-analyze: the offline analysis passes and the mix assistant's planner
# (ADR-0011 3.4, ADR-0046). On common/model, so Linux-only like it.
add_subdirectory(plugins/analyze)
endif()

# The neutral modules and their tests, as one target: what CI's Portable
# (MSVC) job builds, so a new test target there needs no edit to ci.yml.
sonora_define_portable_target()

# ADR-0007 §7: every test carries its module's ctest label, so a squad runs
# `ctest -L core` or `-L ui` for a quick loop. Appended, so labels a module
# sets itself ("slow", "plugins") stay. Needs CMake 3.28 (the TESTS directory
# property); older CMake builds the same tests without the module labels.
# The gate is still the unlabelled, full ctest.
function(sonora_label_module_tests dir label)
  if(CMAKE_VERSION VERSION_LESS 3.28)
    return()
  endif()
  set(_dirs "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
  while(_dirs)
    list(POP_FRONT _dirs _d)
    get_property(_tests DIRECTORY "${_d}" PROPERTY TESTS)
    foreach(_t IN LISTS _tests)
      set_property(TEST "${_t}" DIRECTORY "${_d}" APPEND PROPERTY LABELS "${label}")
    endforeach()
    get_property(_subs DIRECTORY "${_d}" PROPERTY SUBDIRECTORIES)
    list(APPEND _dirs ${_subs})
  endwhile()
endfunction()

# ADR-0071: the Qt interface and the neutral modules it links (ui/tui and
# anything POSIX stay out). sonora_portable_ui is what the nightly builds.
if(SONORA_PORTABLE_UI AND NOT SONORA_PLATFORM STREQUAL "linux" AND NOT SONORA_PLUGINS_ONLY)
  add_subdirectory(contracts/api)
  add_subdirectory(ui/keymap)
  add_subdirectory(ui/client)
  add_subdirectory(ui/qt)
  _sonora_collect_targets(${CMAKE_SOURCE_DIR}/ui/qt _ui_targets)
  set(_ui_deps)
  foreach(_t ${_ui_targets})
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "STATIC_LIBRARY")
      list(APPEND _ui_deps ${_t})
    endif()
  endforeach()
  add_custom_target(sonora_portable_ui DEPENDS ${_ui_deps})
  sonora_label_module_tests(ui/keymap ui)
  sonora_label_module_tests(ui/client ui)
  sonora_label_module_tests(ui/qt ui)
endif()

# Everything below is POSIX or Qt: the neutral build stops here, and so does
# the plugins-only one.
if(NOT SONORA_PLATFORM STREQUAL "linux" OR SONORA_PLUGINS_ONLY)
  sonora_label_module_tests(common/base base)
  sonora_label_module_tests(common/json json)
  sonora_label_module_tests(common/platform platform)
  sonora_label_module_tests(common/dsp dsp)
  sonora_label_module_tests(plugins/native native)
  return()
endif()
add_subdirectory(contracts/api)
add_subdirectory(contracts/backend)
# Control surface events in, feedback out (ADR-0007 4.4, ADR-0040).
add_subdirectory(contracts/surfaces)
# The engine <-> plugin host shared-memory layout, accepted as layout 1
# (ADR-0012 §5).
add_subdirectory(contracts/plugin)
# Optional: skipped, with one status line, where lilv is not installed.
# tools/build-plugins.sh builds and tests it. Before engine/plugin_client,
# whose end-to-end tests run it when it is built.
add_subdirectory(plugins/host)
add_subdirectory(engine/core)
# Offline converters (ADR-0007 M10); the engine never links them.
add_subdirectory(interchange)
add_subdirectory(drivers/support)
add_subdirectory(drivers/jack)
# ALSA direct (ADR-0053). Its logic builds and tests everywhere; libasound is optional.
add_subdirectory(drivers/alsa)
# MCU control surface over a MIDI port (ADR-0055); after contracts/api.
add_subdirectory(drivers/surfaces)
# Platform-neutral (ADR-0008): the backend engine tests run on, any OS.
add_subdirectory(drivers/fake)
# The engine's side of the plugin boundary (ADR-0012 checklist 4). Linked
# by engine/core, added above, for PluginChainRunner; it links
# common/platform PRIVATE so engine/core still cannot include a platform
# header (ADR-0007 §1).
add_subdirectory(engine/plugin_client)
# The plug-in editor service contract and its fake (ADR-0088); header-only.
add_subdirectory(engine/editor_service)
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

# Module labels (see sonora_label_module_tests above). The neutral modules
# are labelled here too; the early return above covers the neutral build.
sonora_label_module_tests(common/base base)
sonora_label_module_tests(common/json json)
sonora_label_module_tests(common/platform platform)
sonora_label_module_tests(common/dsp dsp)
sonora_label_module_tests(plugins/native native)
sonora_label_module_tests(common/audiofile audiofile)
sonora_label_module_tests(common/midi midi)
sonora_label_module_tests(common/model model)
sonora_label_module_tests(common/session_files session_files)
sonora_label_module_tests(contracts/api contracts)
sonora_label_module_tests(contracts/backend contracts)
sonora_label_module_tests(contracts/surfaces contracts)
sonora_label_module_tests(contracts/plugin contracts)
sonora_label_module_tests(plugins/host pluginhost)
sonora_label_module_tests(engine/core core)
sonora_label_module_tests(engine/plugin_client plugin_client)
sonora_label_module_tests(engine/editor_service control)
sonora_label_module_tests(engine/control control)
sonora_label_module_tests(engine/app app)
sonora_label_module_tests(drivers/support drivers)
sonora_label_module_tests(drivers/jack drivers)
sonora_label_module_tests(drivers/fake drivers)
sonora_label_module_tests(ui/keymap ui)
sonora_label_module_tests(ui/tui ui)
sonora_label_module_tests(ui/client ui)
sonora_label_module_tests(ui/qt ui)
