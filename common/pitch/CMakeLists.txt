# common/launch (ADR-0164) hooks in here while the root CMakeLists.txt is
# held by other features; it has no dependency on this module.
if(EXISTS "${CMAKE_SOURCE_DIR}/common/launch/CMakeLists.txt" AND NOT TARGET sonora_launch)
  add_subdirectory("${CMAKE_SOURCE_DIR}/common/launch" "${CMAKE_BINARY_DIR}/common/launch")
endif()

# Pitch analysis and planning (time.pitch, ADR-0143 section 3). Pure standard
# C++17, off the audio thread: a YIN-class tracker, note segmentation, the
# PitchPlan that produces the PitchEditSpec of contracts/api/pitch.h, and the
# PitchShifter interface with a fake. No Rubber Band, no ONNX.
#
# pitch.h is header-only, so its include directory is added by path instead of
# linking sonora_api: this directory is added from plugins/analyze's hook, in
# builds where that target may not exist.
add_library(sonora_pitch STATIC
  src/yin_tracker.cpp
  src/segment.cpp
  src/plan.cpp
  src/plan_pieces.cpp
  src/fake_shifter.cpp)
target_include_directories(sonora_pitch PUBLIC include
  "${CMAKE_SOURCE_DIR}/contracts/api/include")
target_compile_features(sonora_pitch PUBLIC cxx_std_17)

foreach(_t tracker segment plan shifter)
  add_executable(pitch_${_t}_test tests/test_${_t}.cpp)
  target_link_libraries(pitch_${_t}_test PRIVATE sonora_pitch)
  add_test(NAME pitch_${_t} COMMAND pitch_${_t}_test)
endforeach()
