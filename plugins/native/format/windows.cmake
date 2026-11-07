# The Windows VST3 bundles (the plugin package, owner 2026-11-05), included by
# format/CMakeLists.txt when SONORA_WINDOWS_VST3 is ON under MSVC. The same
# sonora_native_plugin() call as the Linux one gives each processor a stereo
# and a "-mono" sonora-<name>[-mono].vst3 bundle:
#   <bundle>/Contents/x86_64-win/<bundle name>.vst3   the DLL
#   <bundle>/Contents/Resources/descriptor.json
# No LV2 here (the LV2 spec has no Windows convention we ship) and no
# bundle driver or validator: both are POSIX (dlopen); the DLL's exports are
# checked by tools/package-plugins.ps1.
set(SONORA_DPF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dpf CACHE INTERNAL "")
set(SONORA_FORMAT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
set(SONORA_NATIVE_VST3_DIR ${CMAKE_BINARY_DIR}/native/vst3 CACHE INTERNAL "")
set(SONORA_VST3_ARCH x86_64-win CACHE INTERNAL "")

add_custom_target(sonora_native_bundles ALL)

option(SONORA_INSTALL_EXPERIMENTAL "Install bundles marked EXPERIMENTAL" OFF)
function(sonora_native_plugin name)
  cmake_parse_arguments(P "TIMEPOS;SIDECHAIN;EXPERIMENTAL;FILES;MEMORYLESS" "FACTORY;DISPLAY;UID;MONO_UID;LV2_CATEGORY;VST3_CATEGORIES" "LINK" ${ARGN})
  foreach(uid "${P_UID}" "${P_MONO_UID}")
    string(LENGTH "${uid}" uid_len)
    if(NOT uid_len EQUAL 4)
      message(FATAL_ERROR "sonora_native_plugin(${name}): UID and MONO_UID must be 4 characters")
    endif()
  endforeach()
  if(P_UID STREQUAL P_MONO_UID)
    message(FATAL_ERROR "sonora_native_plugin(${name}): UID and MONO_UID must differ")
  endif()
  _sonora_native_win_bundle(${name} "" 2 "${P_UID}" "${P_DISPLAY}")
  _sonora_native_win_bundle(${name} "-mono" 1 "${P_MONO_UID}" "${P_DISPLAY} (mono)")
endfunction()

function(_sonora_native_win_bundle name suffix channels uid display)
  set(b ${name}${suffix})
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/dpf${suffix})
  set(SONORA_NAME ${name})
  set(SONORA_VARIANT "${channels} channel(s)")
  set(SONORA_LV2_URI "urn:sonora:native:${b}")
  set(SONORA_FACTORY ${P_FACTORY})
  set(SONORA_DISPLAY_NAME "${display}")
  set(SONORA_UID ${uid})
  set(SONORA_LV2_CATEGORY ${P_LV2_CATEGORY})
  set(SONORA_VST3_CATEGORIES ${P_VST3_CATEGORIES})
  set(SONORA_CHANNELS ${channels})
  if(P_SIDECHAIN)
    set(SONORA_SIDECHAIN_CHANNELS ${channels})
  else()
    set(SONORA_SIDECHAIN_CHANNELS 0)
  endif()
  if(P_FILES)
    set(SONORA_FILE_STATE 1)
  else()
    set(SONORA_FILE_STATE 0)
  endif()
  if(P_TIMEPOS)
    set(SONORA_TIMEPOS 1)
  else()
    set(SONORA_TIMEPOS 0)
  endif()
  configure_file(${SONORA_FORMAT_DIR}/src/dpf/DistrhoPluginInfo.h.in ${gen}/DistrhoPluginInfo.h @ONLY)
  configure_file(${SONORA_FORMAT_DIR}/src/dpf/entry.cpp.in ${gen}/entry.cpp @ONLY)

  set(dpf_sources ${SONORA_DPF_DIR}/distrho/DistrhoPluginMain.cpp)
  # DPF's own code is third-party: its warnings are not ours to fix.
  set_source_files_properties(${dpf_sources} PROPERTIES COMPILE_OPTIONS "/w")

  set(vst3_bundle ${SONORA_NATIVE_VST3_DIR}/sonora-${b}.vst3)
  set(t sonora-${b}-vst3)
  add_library(${t} MODULE ${dpf_sources} ${SONORA_FORMAT_DIR}/src/dpf/plugin.cpp ${gen}/entry.cpp)
  target_compile_definitions(${t} PRIVATE DISTRHO_PLUGIN_TARGET_VST3 SONORA_DPF_QUIET)
  target_include_directories(${t} PRIVATE ${gen})
  target_include_directories(${t} SYSTEM PRIVATE ${SONORA_DPF_DIR}/distrho)
  target_link_libraries(${t} PRIVATE ${P_LINK} sonora_native_${name} sonora_native_format)
  # A MODULE library is a LIBRARY artifact even where it is a DLL.
  set_target_properties(${t} PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME sonora-${b}
                        LIBRARY_OUTPUT_DIRECTORY ${vst3_bundle}/Contents/${SONORA_VST3_ARCH})
  add_dependencies(sonora_native_bundles ${t})

  # The JSON descriptor the Sonora UI builds the editor from (ADR-0011 2.6),
  # from the same table, as on Linux (bundle_files.cpp).
  set(gen_t sonora-${b}-bundle-files)
  add_executable(${gen_t} ${dpf_sources} ${SONORA_FORMAT_DIR}/src/dpf/plugin.cpp ${gen}/entry.cpp
                 ${SONORA_FORMAT_DIR}/src/dpf/bundle_files.cpp)
  target_compile_definitions(${gen_t} PRIVATE DISTRHO_PLUGIN_TARGET_LV2)
  target_include_directories(${gen_t} PRIVATE ${gen})
  target_include_directories(${gen_t} SYSTEM PRIVATE ${SONORA_DPF_DIR}/distrho)
  target_link_libraries(${gen_t} PRIVATE ${P_LINK} sonora_native_${name} sonora_native_format)
  set_target_properties(${gen_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${vst3_bundle}/Contents/Resources
    COMMAND $<TARGET_FILE:${gen_t}> --descriptor ${vst3_bundle}/Contents/Resources/descriptor.json
    COMMENT "Generating the descriptor of sonora-${b}.vst3"
    VERBATIM)
  add_dependencies(${t} ${gen_t})

  # EXPERIMENTAL: built, not installed, until the processor holds its
  # ADR-0011 3.5 CPU class (a processor over its class does not ship).
  if(NOT P_EXPERIMENTAL OR SONORA_INSTALL_EXPERIMENTAL)
    install(DIRECTORY ${vst3_bundle} DESTINATION lib/vst3)
  endif()
endfunction()
