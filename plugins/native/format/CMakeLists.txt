# The format adapters (ADR-0011 §1.2, stage N1): each native processor as an
# LV2 bundle and a VST3 bundle, through DPF behind our own interface.
#
# - sonora_native_format: the format-neutral half (Bridge). Neutral C++17
#   (ADR-0008), tested here without any plugin format.
# - sonora_native_plugin(): what a processor's CMakeLists calls to get its
#   two bundles. The DPF glue (src/dpf/) is compiled per bundle and format,
#   because DPF describes a plugin with per-bundle macros.
#
# DPF is vendored under third_party/dpf at a pinned commit, the LV2 and VST3
# parts only (third_party/dpf/SONORA.md: commit, licence, what was left
# out). ISC, as ADR-0011 §5 allows until the owner rules on the suite's
# licence. No processor sees it: replacing it is this directory only.
#
# Edges (ADR-0011 §2.2): plugins/native/sdk. The bundle targets are created
# in each processor's directory and link that processor and this library:
# the plugins/native/<name> -> plugins/native/format edge.

add_library(sonora_native_format STATIC src/bridge.cpp)
target_include_directories(sonora_native_format PUBLIC include)
target_link_libraries(sonora_native_format PUBLIC sonora_native_sdk)

# A processor of the test's own, so the adapter is tested without linking a
# real processor (format must not depend on plugins/native/<name>).
add_executable(test_native_format tests/test_bridge.cpp)
target_link_libraries(test_native_format PRIVATE sonora_native_format Threads::Threads)
add_test(NAME native_format COMMAND test_native_format)

# Bundles are Linux-only for now, like the host that loads them. DPF itself
# builds on Windows and macOS; turning those on is a packaging decision for
# a Windows Sonora (ADR-0008), not a code change here.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "plugins/native/format: LV2 and VST3 bundles are built on Linux only; skipped")
  function(sonora_native_plugin)
  endfunction()
  return()
endif()

set(SONORA_DPF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dpf CACHE INTERNAL "")
set(SONORA_FORMAT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
# Where the bundles land: a host test finds them here, and `cmake --install`
# copies them to the standard search paths (lib/lv2, lib/vst3).
set(SONORA_NATIVE_LV2_DIR ${CMAKE_BINARY_DIR}/native/lv2 CACHE INTERNAL "")
set(SONORA_NATIVE_VST3_DIR ${CMAKE_BINARY_DIR}/native/vst3 CACHE INTERNAL "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(SONORA_VST3_ARCH x86_64-linux CACHE INTERNAL "")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(SONORA_VST3_ARCH aarch64-linux CACHE INTERNAL "")
else()
  set(SONORA_VST3_ARCH ${CMAKE_SYSTEM_PROCESSOR}-linux CACHE INTERNAL "")
endif()

# The N1 checkpoint's two outside judges (ADR-0011 §1.2): lv2lint and
# Steinberg's VST3 validator, run on every bundle when installed. The
# sonora-plugins image has both (tools/Dockerfile.plugins), and
# tools/build-plugins.sh sets SONORA_REQUIRE_PLUGIN_VALIDATORS so CI fails
# rather than skipping them; a developer's machine without them still builds.
find_program(SONORA_LV2LINT lv2lint)
find_program(SONORA_VST3_VALIDATOR validator)
option(SONORA_REQUIRE_PLUGIN_VALIDATORS "Fail the configure step when lv2lint or the VST3 validator is missing" OFF)
if(SONORA_REQUIRE_PLUGIN_VALIDATORS AND (NOT SONORA_LV2LINT OR NOT SONORA_VST3_VALIDATOR))
  message(FATAL_ERROR "plugins/native/format: SONORA_REQUIRE_PLUGIN_VALIDATORS is ON and lv2lint "
                      "or the VST3 validator is missing (tools/Dockerfile.plugins builds both)")
endif()

# Every bundle target, so tests can depend on all of them at once.
add_custom_target(sonora_native_bundles ALL)

# sonora_native_plugin(<name> FACTORY <fn> DISPLAY <name> UID <4 chars>
#                      LV2_CATEGORY <uri> VST3_CATEGORIES <list> [TIMEPOS])
#
# Builds sonora-<name>.lv2 (binary plus the TTL DPF generates from the
# parameter table) and sonora-<name>.vst3 from sonora_native_<name>.
# <fn> is the processor's factory in namespace sonora::native, declared in
# sonora/native/<name>.h. <uid> is the VST3 class ID's plugin word: never
# change it, saved sessions find the plugin by it.
function(sonora_native_plugin name)
  cmake_parse_arguments(P "TIMEPOS" "FACTORY;DISPLAY;UID;LV2_CATEGORY;VST3_CATEGORIES" "" ${ARGN})
  string(LENGTH "${P_UID}" uid_len)
  if(NOT uid_len EQUAL 4)
    message(FATAL_ERROR "sonora_native_plugin(${name}): UID must be 4 characters")
  endif()
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/dpf)
  set(SONORA_NAME ${name})
  set(SONORA_FACTORY ${P_FACTORY})
  set(SONORA_DISPLAY_NAME "${P_DISPLAY}")
  set(SONORA_UID ${P_UID})
  set(SONORA_LV2_CATEGORY ${P_LV2_CATEGORY})
  set(SONORA_VST3_CATEGORIES ${P_VST3_CATEGORIES})
  if(P_TIMEPOS)
    set(SONORA_TIMEPOS 1)
  else()
    set(SONORA_TIMEPOS 0)
  endif()
  configure_file(${SONORA_FORMAT_DIR}/src/dpf/DistrhoPluginInfo.h.in ${gen}/DistrhoPluginInfo.h @ONLY)
  configure_file(${SONORA_FORMAT_DIR}/src/dpf/entry.cpp.in ${gen}/entry.cpp @ONLY)

  # A plugin is a shared object: everything linked into it is
  # position-independent. Code generation for the arithmetic is unchanged,
  # so the golden files still hold.
  foreach(dep sonora_base sonora_json sonora_dsp sonora_native_sdk sonora_native_format
              sonora_native_${name})
    set_property(TARGET ${dep} PROPERTY POSITION_INDEPENDENT_CODE ON)
  endforeach()

  set(dpf_sources ${SONORA_DPF_DIR}/distrho/DistrhoPluginMain.cpp)
  # DPF's own code is third-party: its warnings are not ours to fix.
  set_source_files_properties(${dpf_sources} PROPERTIES COMPILE_OPTIONS "-w")

  foreach(fmt lv2 vst3)
    string(TOUPPER ${fmt} FMT)
    set(t sonora-${name}-${fmt})
    add_library(${t} MODULE ${dpf_sources} ${SONORA_FORMAT_DIR}/src/dpf/plugin.cpp ${gen}/entry.cpp)
    target_compile_definitions(${t} PRIVATE DISTRHO_PLUGIN_TARGET_${FMT})
    target_include_directories(${t} PRIVATE ${gen})
    target_include_directories(${t} SYSTEM PRIVATE ${SONORA_DPF_DIR}/distrho)
    target_link_libraries(${t} PRIVATE sonora_native_${name} sonora_native_format)
    # Only the format's entry points are exported; lv2lint's symbol check
    # holds us to it.
    set_target_properties(${t} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden
                          VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
    add_dependencies(sonora_native_bundles ${t})
  endforeach()

  set(lv2_bundle ${SONORA_NATIVE_LV2_DIR}/sonora-${name}.lv2)
  set_target_properties(sonora-${name}-lv2 PROPERTIES
    OUTPUT_NAME sonora-${name}_dsp
    LIBRARY_OUTPUT_DIRECTORY ${lv2_bundle})
  target_link_options(sonora-${name}-lv2 PRIVATE
    -Wl,--version-script=${SONORA_FORMAT_DIR}/src/dpf/lv2.version -Wl,--no-undefined)

  set(vst3_bundle ${SONORA_NATIVE_VST3_DIR}/sonora-${name}.vst3)
  set_target_properties(sonora-${name}-vst3 PROPERTIES
    OUTPUT_NAME sonora-${name}
    LIBRARY_OUTPUT_DIRECTORY ${vst3_bundle}/Contents/${SONORA_VST3_ARCH})
  target_link_options(sonora-${name}-vst3 PRIVATE
    -Wl,--version-script=${SONORA_DPF_DIR}/utils/symbols/vst3.version -Wl,--no-undefined)

  # The generated files (ADR-0011 §2.4, one table): the LV2 TTL, by DPF's
  # exporter, and the JSON descriptor the Sonora UI builds the editor from
  # (§2.6), in both bundles. Built into a small executable from the same
  # sources rather than dlopen()ing the bundle: no platform loader in the
  # build, and it runs under the sanitizers like everything else.
  set(gen_t sonora-${name}-bundle-files)
  add_executable(${gen_t} ${dpf_sources} ${SONORA_FORMAT_DIR}/src/dpf/plugin.cpp ${gen}/entry.cpp
                 ${SONORA_FORMAT_DIR}/src/dpf/bundle_files.cpp)
  target_compile_definitions(${gen_t} PRIVATE DISTRHO_PLUGIN_TARGET_LV2)
  target_include_directories(${gen_t} PRIVATE ${gen})
  target_include_directories(${gen_t} SYSTEM PRIVATE ${SONORA_DPF_DIR}/distrho)
  target_link_libraries(${gen_t} PRIVATE sonora_native_${name} sonora_native_format)
  set_target_properties(${gen_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  add_custom_command(TARGET sonora-${name}-lv2 POST_BUILD
    COMMAND $<TARGET_FILE:${gen_t}> --lv2 sonora-${name}_dsp --descriptor ${lv2_bundle}/descriptor.json
    WORKING_DIRECTORY ${lv2_bundle}
    COMMENT "Generating the TTL and descriptor of sonora-${name}.lv2"
    VERBATIM)
  add_dependencies(sonora-${name}-lv2 ${gen_t})
  add_custom_command(TARGET sonora-${name}-vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${vst3_bundle}/Contents/Resources
    COMMAND $<TARGET_FILE:${gen_t}> --descriptor ${vst3_bundle}/Contents/Resources/descriptor.json
    COMMENT "Generating the descriptor of sonora-${name}.vst3"
    VERBATIM)
  add_dependencies(sonora-${name}-vst3 ${gen_t})

  if(SONORA_LV2LINT)
    add_test(NAME native_lv2lint_${name}
             COMMAND ${SONORA_LV2LINT} -I ${lv2_bundle}/ urn:sonora:native:${name})
    set_tests_properties(native_lv2lint_${name} PROPERTIES LABELS plugins)
  endif()
  if(SONORA_VST3_VALIDATOR)
    add_test(NAME native_vst3_validator_${name} COMMAND ${SONORA_VST3_VALIDATOR} ${vst3_bundle})
    set_tests_properties(native_vst3_validator_${name} PROPERTIES LABELS plugins)
  endif()

  install(DIRECTORY ${lv2_bundle} DESTINATION lib/lv2)
  install(DIRECTORY ${vst3_bundle} DESTINATION lib/vst3)

  # For the host tests and the validators (plugins/host/CMakeLists.txt):
  # where the bundles are, and the VST3 class ID the host loads by.
  # DPF builds the 16-byte ID from four 32-bit words, "DPF " "clas" <uid>
  # <brand>, stored in host byte order: on little-endian Linux each word's
  # characters come out reversed.
  string(SUBSTRING "${P_UID}" 0 1 c0)
  string(SUBSTRING "${P_UID}" 1 1 c1)
  string(SUBSTRING "${P_UID}" 2 1 c2)
  string(SUBSTRING "${P_UID}" 3 1 c3)
  string(HEX "${c3}${c2}${c1}${c0}" uid_hex)
  string(TOUPPER "2046504473616C63${uid_hex}61726E53" cid)
  set_property(GLOBAL APPEND PROPERTY SONORA_NATIVE_PLUGINS ${name})
  set_property(GLOBAL PROPERTY SONORA_NATIVE_${name}_VST3_ID "vst3:${cid}")
  set_property(GLOBAL PROPERTY SONORA_NATIVE_${name}_LV2_URI "urn:sonora:native:${name}")
endfunction()
