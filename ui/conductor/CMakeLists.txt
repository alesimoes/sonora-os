# Conductor-ruler gestures (ADR-0140): pure functions over the tempo map.
# Hooked from ui/groove/CMakeLists.txt until the root CMakeLists can name it
# (ADR-0140 records the move). Depends on sonora_model only; no Qt.
if(TARGET sonora_ui_conductor)
  return()
endif()

add_library(sonora_ui_conductor STATIC src/conductor.cpp)
target_include_directories(sonora_ui_conductor PUBLIC include)
target_link_libraries(sonora_ui_conductor PUBLIC sonora_model)

add_executable(test_ui_conductor tests/test_conductor_gestures.cpp)
target_link_libraries(test_ui_conductor PRIVATE sonora_ui_conductor)
add_test(NAME ui_conductor COMMAND test_ui_conductor)
set_tests_properties(ui_conductor PROPERTIES LABELS ui)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/qt/CMakeLists.txt)
  add_subdirectory(qt)
endif()
