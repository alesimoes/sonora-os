# The Pitch editor (time.pitch, ADR-0143): the notes of a region, the
# operator's edit of each, and Apply as one sample.edit "pitch". Qt-free, on
# common/pitch and contracts/api only; the panel's QML is under qt/, added only
# when it is there. Added from plugins/analyze (ADR-0143 §6) until the root is
# free to add it.
add_library(sonora_ui_pitch STATIC
  src/pitch_engine.cpp
  src/pitch_editor_model.cpp
  src/pitch_apply.cpp
  src/pitch_view.cpp)
target_include_directories(sonora_ui_pitch PUBLIC include)
target_link_libraries(sonora_ui_pitch PUBLIC sonora_pitch)
target_compile_features(sonora_ui_pitch PUBLIC cxx_std_17)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/qt/CMakeLists.txt)
  add_subdirectory(qt)
endif()

# One file per behaviour; ctest -R ui_pitch runs them all. The apply test
# talks to the fake engine (contracts/api/testing), which the root defines
# after this directory: the name resolves when the build is generated.
foreach(_t model view apply)
  add_executable(test_ui_pitch_${_t} tests/test_pitch_${_t}.cpp)
  target_link_libraries(test_ui_pitch_${_t} PRIVATE sonora_ui_pitch)
  add_test(NAME ui_pitch_${_t} COMMAND test_ui_pitch_${_t})
  set_tests_properties(ui_pitch_${_t} PROPERTIES LABELS ui)
endforeach()
target_link_libraries(test_ui_pitch_apply PRIVATE sonora_api_fake)
