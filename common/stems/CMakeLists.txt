# Stem separation contract (ADR-0165): interface, fake, separators registry.
# Hooked from common/groove/CMakeLists.txt until the root file takes it.
file(GLOB _stems_src CONFIGURE_DEPENDS src/*.cpp)
add_library(sonora_stems STATIC ${_stems_src})
target_include_directories(sonora_stems PUBLIC include)
target_compile_features(sonora_stems PUBLIC cxx_std_17)

file(GLOB _stems_tests CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(_f ${_stems_tests})
  get_filename_component(_n ${_f} NAME_WE)
  string(REGEX REPLACE "^test_" "" _n ${_n})
  add_executable(stems_${_n}_test ${_f})
  target_link_libraries(stems_${_n}_test PRIVATE sonora_stems)
  add_test(NAME stems_${_n} COMMAND stems_${_n}_test)
  set_tests_properties(stems_${_n} PROPERTIES LABELS stems)
endforeach()
