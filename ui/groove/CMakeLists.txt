# Groove as an edit (ADR-0115): the tempo.edit groove words, the onset
# source, the saved templates and the Groove panel. A module of its own so
# ui/qt's CMake is untouched: its Qt-free sources join sonora_ui_edit, where
# edit_tempo.cpp (which dispatches the `groove` word) lives, since putting
# them in sonora_ui_session would make sonora_ui_edit need a library above it.
# Nothing here exists where ui/qt was skipped (no Qt 6).
if(NOT TARGET sonora_ui_edit)
  return()
endif()

target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_groove/groove_command.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_groove/onset_source.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_groove/template_store.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/groove_detail.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/groove_apply.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/groove_command.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/onset_source.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/template_store.cpp
)
# PUBLIC: edit_tempo.cpp finds groove_command.h through it (__has_include).
target_include_directories(sonora_ui_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sonora_ui_edit PUBLIC sonora_groove sonora_json)

# The panel's QML as text in a generated header (groove_panel.h). Not a Qt
# resource: qt_add_resources from this directory, on a target ui/qt defines,
# generated nothing, and ui/qt's resource list is not this module's to edit.
# The loader gives it its qrc:/sonora/qml/ address, so SButton and Theme
# resolve as for the QML beside it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/GroovePanel.qml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/qml/GroovePanel.qml SONORA_GROOVE_PANEL_QML)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/groove_panel_qml.inc
  CONTENT "R\"sonoraqml(${SONORA_GROOVE_PANEL_QML})sonoraqml\"\n" @ONLY)
target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_groove/groove_panel.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/groove_panel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/GroovePanel.qml
)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/qml/GroovePanel.qml
  PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(sonora_ui_edit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_executable(test_ui_groove tests/test_groove_ui.cpp)
target_link_libraries(test_ui_groove PRIVATE sonora_ui_edit)
add_test(NAME ui_groove COMMAND test_ui_groove)
set_tests_properties(ui_groove PROPERTIES LABELS ui)
