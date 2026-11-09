# Editing on the Tempo and Meter rulers (ADR-0140), wired onto ui/qt the way
# ui/groove is, so ui/qt's CMake is untouched. The Qt-free part joins
# sonora_ui_edit, beside EditController, which every gesture goes through;
# ConductorRulerItem (sonora_ui_session) and SessionModel find it with
# __has_include. Nothing here exists where ui/qt was skipped (no Qt 6).
if(NOT TARGET sonora_ui_edit)
  return()
endif()

target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/conductor_ruler_edit.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/conductor_ruler_edit.cpp
)
target_include_directories(sonora_ui_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sonora_ui_edit PUBLIC sonora_ui_conductor)

# The editor's QML as text in a generated header, as ui/groove's panel: a
# qt_add_resources from here, on a target ui/qt defines, generates nothing.
# EditWindow gives it its qrc:/sonora/qml/ address, so Theme and SButton
# resolve as for the QML beside them.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/ConductorRulerEdit.qml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/qml/ConductorRulerEdit.qml SONORA_CONDUCTOR_RULER_QML)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/conductor_ruler_edit_qml.inc
  CONTENT "R\"sonoraqml(${SONORA_CONDUCTOR_RULER_QML})sonoraqml\"\n" @ONLY)
target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/conductor_ruler_qml.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/conductor_ruler_qml.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/ConductorRulerEdit.qml
)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/qml/ConductorRulerEdit.qml
  PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(sonora_ui_edit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# The screenshot scenes (sonora-ui-edit-conductor-*.png) register themselves,
# so they join the executable, where the linker keeps a file nothing names.
if(TARGET sonora-ui)
  target_sources(sonora-ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/edit_conductor_edit_scene.cpp)
  target_include_directories(sonora-ui PRIVATE ${CMAKE_SOURCE_DIR}/ui/qt/src/scenes)
endif()

add_executable(test_ui_conductor_edit tests/test_conductor_edit.cpp)
target_link_libraries(test_ui_conductor_edit PRIVATE sonora_ui_edit)
add_test(NAME ui_conductor_edit COMMAND test_ui_conductor_edit)
set_tests_properties(ui_conductor_edit PROPERTIES LABELS ui)
