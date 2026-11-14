# External clock source: select the card's clock and refuse to record on a
# clock that is not locked (ADR-0125). Control thread only; depends on nothing
# but the standard library. Sources and tests are globbed so the impl and
# tests slices add files without touching this one.
file(GLOB SONORA_CLOCK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(sonora_clock STATIC ${SONORA_CLOCK_SOURCES})
target_include_directories(sonora_clock PUBLIC include)

add_library(sonora_clock_testing INTERFACE)
target_include_directories(sonora_clock_testing INTERFACE testing)
target_link_libraries(sonora_clock_testing INTERFACE sonora_clock)

file(GLOB SONORA_CLOCK_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
foreach(_src IN LISTS SONORA_CLOCK_TESTS)
  get_filename_component(_stem "${_src}" NAME_WE)
  add_executable(clock_${_stem} "${_src}")
  target_link_libraries(clock_${_stem} PRIVATE sonora_clock_testing)
  add_test(NAME clock_${_stem} COMMAND clock_${_stem})
  set_tests_properties(clock_${_stem} PROPERTIES LABELS drivers)
endforeach()
