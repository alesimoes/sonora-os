# LTC chase and generate (ADR-0124). Control thread only; the audio-thread
# tap is a later item. Sources and tests are globbed so the codec, driver
# and test slices add files without a CMake edit (plan drivers.sync.ltc §3).
# Hooked from drivers/support until the root list is free; the tests carry
# the drivers label themselves for the same reason.
file(GLOB _ltc_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(sonora_ltc STATIC ${_ltc_sources})
target_include_directories(sonora_ltc PUBLIC include)
target_link_libraries(sonora_ltc PUBLIC sonora_api)

# One executable per test file, so the chase and generator behaviour tests are
# ctest ltc_chase_behaviour and ltc_generator_behaviour; their shared fixtures
# sit in testing/ltc_test_helpers.h (testing is on every test's include path).
file(GLOB _ltc_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
foreach(_src ${_ltc_tests})
  get_filename_component(_stem ${_src} NAME_WE)
  string(REGEX REPLACE "^test_" "" _name ${_stem})
  # Named ltc_<stem> so `ctest -R ltc` runs the module, and the test file
  # test_ltc_contract.cpp is the test ltc_contract.
  string(REGEX REPLACE "^ltc_" "" _name ${_name})
  add_executable(test_ltc_${_name} ${_src})
  target_include_directories(test_ltc_${_name} PRIVATE testing)
  target_link_libraries(test_ltc_${_name} PRIVATE sonora_ltc)
  add_test(NAME ltc_${_name} COMMAND test_ltc_${_name})
  set_tests_properties(ltc_${_name} PROPERTIES LABELS drivers)
endforeach()
