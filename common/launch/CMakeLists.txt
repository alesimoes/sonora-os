# Clip launcher (logic.live_loops, ADR-0164): the launch grid, quantisation
# and the quantised state machine. Pure standard C++17, no module edges;
# advance() runs on the audio thread and never allocates.
#
# Added from common/pitch's hook while the root CMakeLists.txt is held by
# other features; the tests set their own label for the same reason. The
# hook moves to the root once that file frees.
add_library(sonora_launch STATIC
  src/launch_quantize.cpp
  src/state_machine_launcher.cpp)
target_include_directories(sonora_launch PUBLIC include)
target_compile_features(sonora_launch PUBLIC cxx_std_17)

foreach(_t quantize launcher block_edges no_alloc)
  add_executable(launch_${_t}_test tests/test_${_t}.cpp)
  target_link_libraries(launch_${_t}_test PRIVATE sonora_launch)
  add_test(NAME launch_${_t} COMMAND launch_${_t}_test)
  set_tests_properties(launch_${_t} PROPERTIES LABELS launch)
endforeach()

# engine/control's launcher contract (ADR-0224) joins once the root is read
# and only where sonora_control exists: engine/control/CMakeLists.txt is held
# by other features. Moves there when that file frees.
if(EXISTS "${CMAKE_SOURCE_DIR}/engine/control/launch.cmake")
  if(CMAKE_VERSION VERSION_LESS 3.19)
    message(WARNING "engine/control launch contract needs CMake 3.19 (cmake_language DEFER); not built")
  else()
    function(_sonora_launch_join_control)
      if(TARGET sonora_control)
        include("${CMAKE_SOURCE_DIR}/engine/control/launch.cmake")
      endif()
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _sonora_launch_join_control)
  endif()
endif()
