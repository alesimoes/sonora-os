# tempo.edit detect (ADR-0114): a module of its own, its Qt-free sources
# joining sonora_ui_edit where edit_tempo.cpp dispatches the word, as
# ui/groove does. Absent where ui/qt is skipped.
if(NOT TARGET sonora_ui_edit)
  return()
endif()

target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_beat/beat_command.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/beat_command.cpp
)
target_include_directories(sonora_ui_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sonora_ui_edit PUBLIC sonora_analyze sonora_platform)

add_executable(test_ui_beat tests/test_beat_command.cpp)
target_link_libraries(test_ui_beat PRIVATE sonora_ui_edit)
add_test(NAME ui_beat COMMAND test_ui_beat)
set_tests_properties(ui_beat PROPERTIES LABELS ui)
