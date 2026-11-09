# The Smart Controls panel (ADR-0117 §6). Reached from ui/smart_controls by a
# deferred call once ui/qt has defined sonora_ui_session, so no file of ui/qt
# is edited: the sources join that library (absolute paths, as the target
# lives in another directory) and the QML joins its resources under the
# same qrc:/sonora/qml/ prefix, beside Knob.qml and SButton.qml.
set(_sc_qt ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(sonora_ui_session PRIVATE
  ${_sc_qt}/src/insert_param_sink.h ${_sc_qt}/src/insert_param_sink.cpp
  ${_sc_qt}/src/smart_controls_model.h ${_sc_qt}/src/smart_controls_model.cpp
  ${_sc_qt}/src/smart_controls_learn.cpp
)
target_include_directories(sonora_ui_session PUBLIC ${_sc_qt}/src)
# The insert controller is the sink's only way in (its public gesture); it
# sits below the session library, so this adds no cycle.
target_link_libraries(sonora_ui_session PUBLIC sonora_ui_inserts sonora_smart_controls)
qt_add_resources(sonora_ui_session sonora_smart_controls_qml
  PREFIX /sonora
  FILES qml/SmartControlsPanel.qml
)

# The model on a held document and catalog, then the panel loaded from the
# resources the application gets.
add_executable(test_ui_smart_controls tests/test_smart_controls_qt.cpp)
target_link_libraries(test_ui_smart_controls PRIVATE sonora_ui_qt Qt6::Test)
add_test(NAME ui_smart_controls COMMAND test_ui_smart_controls)
set_tests_properties(ui_smart_controls PROPERTIES
  LABELS ui
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
