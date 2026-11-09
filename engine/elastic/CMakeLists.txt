# audio.elastic (ADR-0136): the checks, the worker job, the edit and the
# Rubber Band adapter. Compiled into sonora_control, whose server installs the
# command, so engine/control's own CMakeLists.txt is left as it is. Nothing
# here runs on the audio thread.
set(_elastic ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(sonora_control PRIVATE
  ${_elastic}/src/elastic_plan.cpp
  ${_elastic}/src/elastic_render.cpp
  ${_elastic}/src/elastic_job.cpp
  ${_elastic}/src/elastic_commands.cpp
  ${_elastic}/src/peak_onsets.cpp)
target_include_directories(sonora_control PUBLIC ${_elastic}/include)
target_link_libraries(sonora_control PUBLIC sonora_stretch)

# Optional (§3): without the library the engine neither installs the command
# nor announces the capability, rather than stretching badly.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(RUBBERBAND QUIET IMPORTED_TARGET GLOBAL rubberband)
endif()
if(RUBBERBAND_FOUND)
  message(STATUS "audio.elastic: Rubber Band ${RUBBERBAND_VERSION}")
  target_sources(sonora_control PRIVATE ${_elastic}/src/rubberband_stretcher.cpp)
  target_link_libraries(sonora_control PRIVATE PkgConfig::RUBBERBAND)
else()
  message(STATUS "audio.elastic: rubberband not found; the command is not built in")
  target_sources(sonora_control PRIVATE ${_elastic}/src/no_library_stretcher.cpp)
endif()

add_executable(test_elastic_edit tests/test_elastic_edit.cpp tests/elastic_rig.cpp)
target_link_libraries(test_elastic_edit PRIVATE sonora_control)
add_test(NAME elastic_edit COMMAND test_elastic_edit)

add_executable(test_elastic_commands tests/test_elastic_commands.cpp tests/elastic_rig.cpp)
target_link_libraries(test_elastic_commands PRIVATE sonora_control)
add_test(NAME elastic_commands COMMAND test_elastic_commands)

add_executable(test_elastic_onsets tests/test_elastic_onsets.cpp)
target_link_libraries(test_elastic_onsets PRIVATE sonora_control)
add_test(NAME elastic_onsets COMMAND test_elastic_onsets)

add_executable(test_elastic_partial_copy tests/test_elastic_partial_copy.cpp)
target_link_libraries(test_elastic_partial_copy PRIVATE sonora_control)
add_test(NAME elastic_partial_copy COMMAND test_elastic_partial_copy)

if(RUBBERBAND_FOUND)
  add_executable(test_elastic_rubberband tests/test_elastic_rubberband.cpp)
  target_link_libraries(test_elastic_rubberband PRIVATE sonora_control)
  add_test(NAME elastic_rubberband COMMAND test_elastic_rubberband)
endif()
