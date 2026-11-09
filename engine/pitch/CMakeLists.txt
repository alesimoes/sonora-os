# time.pitch (ADR-0143 sections 4 and 6): the `pitch` verb of sample.edit.
# Added by the deferred hook in plugins/analyze/CMakeLists.txt once the root is
# read and only when sonora_control exists; this file joins its sources to
# sonora_control (engine/control/CMakeLists.txt is held by other features),
# so the paths are absolute. Moves to engine/control when that file frees.
set(_pitch_dir "${CMAKE_CURRENT_SOURCE_DIR}")

target_sources(sonora_control PRIVATE
  "${_pitch_dir}/src/pitch_render.cpp"
  "${_pitch_dir}/src/shifter_factory.cpp")
target_include_directories(sonora_control PUBLIC "${_pitch_dir}/include")
target_link_libraries(sonora_control PUBLIC sonora_pitch)

# Rubber Band is optional: without it the shifter is not built and the
# `pitch` capability is off. GLOBAL so sonora_control, defined in another
# directory, resolves the imported target when it is linked.
set(SONORA_PITCH_RUBBERBAND 0)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SONORA_RUBBERBAND QUIET IMPORTED_TARGET GLOBAL rubberband)
  if(SONORA_RUBBERBAND_FOUND)
    set(SONORA_PITCH_RUBBERBAND 1)
  endif()
endif()
if(SONORA_PITCH_RUBBERBAND)
  message(STATUS "engine/pitch: Rubber Band ${SONORA_RUBBERBAND_VERSION}; pitch editing on")
  target_sources(sonora_control PRIVATE "${_pitch_dir}/src/rubberband_shifter.cpp")
  target_link_libraries(sonora_control PRIVATE PkgConfig::SONORA_RUBBERBAND)
else()
  message(STATUS "engine/pitch: Rubber Band not found; pitch editing off")
endif()
target_compile_definitions(sonora_control PRIVATE SONORA_PITCH_RUBBERBAND=${SONORA_PITCH_RUBBERBAND})

# The root labels by directory and does not know this one; label "engine"
# for the task's ctest -L engine, "control" as the tests of the code they run.
add_executable(test_sample_edit_pitch
  "${CMAKE_SOURCE_DIR}/engine/control/tests/test_sample_edit_pitch.cpp")
target_link_libraries(test_sample_edit_pitch PRIVATE sonora_control)
target_compile_definitions(test_sample_edit_pitch PRIVATE
  SONORA_PITCH_RUBBERBAND=${SONORA_PITCH_RUBBERBAND})
add_test(NAME sample_edit_pitch COMMAND test_sample_edit_pitch)
set_tests_properties(sample_edit_pitch PROPERTIES LABELS "control;engine")

if(SONORA_PITCH_RUBBERBAND)
  add_executable(test_pitch_rubberband tests/test_rubberband_shifter.cpp
    src/rubberband_shifter.cpp)
  target_include_directories(test_pitch_rubberband PRIVATE src)
  target_link_libraries(test_pitch_rubberband PRIVATE sonora_pitch PkgConfig::SONORA_RUBBERBAND)
  add_test(NAME pitch_rubberband COMMAND test_pitch_rubberband)
  set_tests_properties(pitch_rubberband PROPERTIES LABELS "control;engine")
endif()
