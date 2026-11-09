# The Smart Controls panel (ADR-0117 §6). Included (not added as a
# subdirectory: CMake refuses that in deferred execution) from
# ui/smart_controls once ui/qt has defined sonora_ui_session, so no file of
# ui/qt is edited. This runs in the top-level directory's scope: every path
# is absolute. The sources join sonora_ui_session and the QML joins its
# resources under qrc:/sonora/qml/, beside Knob.qml and SButton.qml.
set(_sc_qt ${CMAKE_CURRENT_LIST_DIR})
# Qt's imported targets (rcc among them) are visible only where Qt was
# found; ui/qt found it, so this finds the same one for this scope.
find_package(Qt6 6.7 QUIET COMPONENTS Core Gui Qml Quick)
set(_sc_qml ${_sc_qt}/qml/SmartControlsPanel.qml)

target_sources(sonora_ui_session PRIVATE
  ${_sc_qt}/src/insert_param_sink.h ${_sc_qt}/src/insert_param_sink.cpp
  ${_sc_qt}/src/smart_controls_model.h ${_sc_qt}/src/smart_controls_model.cpp
  ${_sc_qt}/src/smart_controls_learn.cpp
)
target_include_directories(sonora_ui_session PUBLIC ${_sc_qt}/src)
# The insert controller is the sink's only way in (its public gesture); it
# sits below the session library, so this adds no cycle.
target_link_libraries(sonora_ui_session PUBLIC sonora_ui_inserts sonora_smart_controls)
set_source_files_properties(${_sc_qml} PROPERTIES
  QT_RESOURCE_ALIAS qml/SmartControlsPanel.qml
  HEADER_FILE_ONLY ON)
qt_add_resources(sonora_ui_session sonora_smart_controls_qml
  PREFIX /sonora
  FILES ${_sc_qml}
)

# The model on a held document and catalog, then the panel loaded from the
# resources the application gets. The QML is listed so that the micro-build
# gate maps a change to it to this test.
add_executable(test_ui_smart_controls ${_sc_qt}/tests/test_smart_controls_qt.cpp ${_sc_qml})
target_link_libraries(test_ui_smart_controls PRIVATE sonora_ui_qt)
add_test(NAME ui_smart_controls COMMAND test_ui_smart_controls)
set_tests_properties(ui_smart_controls PROPERTIES
  LABELS ui
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
