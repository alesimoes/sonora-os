# Clip launcher (logic.live_loops, ADR-0164): the launch grid, quantisation
# and the quantised state machine. Pure standard C++17, no module edges;
# advance() runs on the audio thread and never allocates.
#
# Added from common/pitch's hook while the root CMakeLists.txt is held by
# other features; the tests set their own label for the same reason. The
# hook moves to the root once that file frees.
add_library(sonora_launch STATIC
  src/launch_bounce.cpp
  src/launch_clock.cpp
  src/launch_mirror.cpp
  src/launch_quantize.cpp
  src/state_machine_launcher.cpp)
target_include_directories(sonora_launch PUBLIC include)
target_compile_features(sonora_launch PUBLIC cxx_std_17)

foreach(_t quantize launcher block_edges no_alloc clock record bounce)
  add_executable(launch_${_t}_test tests/test_${_t}.cpp)
  target_link_libraries(launch_${_t}_test PRIVATE sonora_launch)
  add_test(NAME launch_${_t} COMMAND launch_${_t}_test)
  set_tests_properties(launch_${_t} PROPERTIES LABELS launch)
endforeach()
