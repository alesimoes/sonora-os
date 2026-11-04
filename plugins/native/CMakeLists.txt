# The native plugin suite (ADR-0011), by discovery: adding a processor is
# adding plugins/native/<name>/ and editing no shared file. README.md has the
# recipe.
#
# Order: the SDK, the format adapters, the measurement harness, then every
# other directory that has a CMakeLists.txt, alphabetically (a processor may
# use the harness's sonora_native_quality_test() and the format layer's
# sonora_native_plugin()), then the harness's executable, which links the
# suites found.
add_subdirectory(sdk)
if(SONORA_PLATFORM STREQUAL "linux")
  add_subdirectory(format)
else()
  # The LV2 and VST3 bundles are Linux-only; the processors still call this.
  function(sonora_native_plugin)
  endfunction()
endif()
add_subdirectory(measure)
# Tests that span processors (the editors' hints and telemetry); no processor.
add_subdirectory(editor_tests)

# CONFIGURE_DEPENDS: a new directory re-runs CMake on the next build, in an
# existing build tree too.
file(GLOB _dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt)
list(SORT _dirs)
set(SONORA_NATIVE_PROCESSORS)
set(SONORA_NATIVE_SUITES)
foreach(_f ${_dirs})
  get_filename_component(_name ${_f} DIRECTORY)
  if(_name STREQUAL "sdk" OR _name STREQUAL "format" OR _name STREQUAL "measure" OR _name STREQUAL "editor_tests")
    continue()
  endif()
  list(APPEND SONORA_NATIVE_PROCESSORS ${_name})
  # A directory with tests/suite.cpp is a processor the harness drives
  # (--processor <name>); one without (ampcab, until its processor lands)
  # is just a library and its tests.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_name}/tests/suite.cpp)
    list(APPEND SONORA_NATIVE_SUITES ${_name})
  endif()
endforeach()
message(STATUS "Native processors: ${SONORA_NATIVE_PROCESSORS}")

# The tables sonora-dsp-measure includes, one line or file per suite, so no
# shared source list is edited either. file(GENERATE) leaves an unchanged
# file alone, so a re-configure does not rebuild.
set(SONORA_NATIVE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_decls)
set(_dispatch)
set(_fx_headers)
set(_fx_decls)
set(_fx_targets)
foreach(_name ${SONORA_NATIVE_SUITES})
  set(_fx ${CMAKE_CURRENT_SOURCE_DIR}/${_name}/tests/host_fixture.inc)
  if(NOT EXISTS ${_fx})
    message(FATAL_ERROR "plugins/native/${_name} has tests/suite.cpp but no tests/host_fixture.inc "
                        "(its Target entry for sonora-dsp-measure --host-fixture; see "
                        "plugins/native/README.md)")
  endif()
  string(APPEND _decls "int ${_name}(const measure::Args&);\n")
  string(APPEND _dispatch
         "    if (which == \"${_name}\") return native::suites::${_name}(args);\n")
  string(APPEND _fx_headers "#include \"sonora/native/${_name}.h\"\n")
  string(APPEND _fx_decls "measure::Config ${_name}_golden_config();\n")
  string(APPEND _fx_targets "#include \"${_fx}\"\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fx})
endforeach()
file(GENERATE OUTPUT ${SONORA_NATIVE_GENERATED_DIR}/suite_decls.inc CONTENT "${_decls}")
file(GENERATE OUTPUT ${SONORA_NATIVE_GENERATED_DIR}/suite_dispatch.inc CONTENT "${_dispatch}")
file(GENERATE OUTPUT ${SONORA_NATIVE_GENERATED_DIR}/fixture_headers.inc CONTENT "${_fx_headers}")
file(GENERATE OUTPUT ${SONORA_NATIVE_GENERATED_DIR}/fixture_golden_decls.inc CONTENT "${_fx_decls}")
file(GENERATE OUTPUT ${SONORA_NATIVE_GENERATED_DIR}/fixture_targets.inc CONTENT "${_fx_targets}")

foreach(_name ${SONORA_NATIVE_PROCESSORS})
  add_subdirectory(${_name})
endforeach()
add_subdirectory(measure/app)

# --- The Portable (MSVC) job's target (.github/workflows/ci.yml). Every
# library and executable made under these directories, so a new test target
# is in the job the moment it exists. Called from the root once everything
# is added. Utility targets and ones the platform cannot build do not exist
# in a SONORA_PLATFORM=neutral configuration.
function(_sonora_collect_targets dir out)
  get_property(_t DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
  get_property(_sub DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(_s ${_sub})
    _sonora_collect_targets(${_s} _more)
    list(APPEND _t ${_more})
  endforeach()
  set(${out} ${_t} PARENT_SCOPE)
endfunction()

function(sonora_define_portable_target)
  set(_all)
  foreach(_d common/json common/dsp plugins/native)
    _sonora_collect_targets(${CMAKE_SOURCE_DIR}/${_d} _t)
    list(APPEND _all ${_t})
  endforeach()
  set(_deps)
  foreach(_t ${_all})
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "STATIC_LIBRARY")
      list(APPEND _deps ${_t})
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _deps)
  list(SORT _deps)
  add_custom_target(sonora_portable DEPENDS ${_deps})
  message(STATUS "sonora_portable: ${_deps}")
endfunction()
