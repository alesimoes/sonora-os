# Elastic Audio in the editor (ADR-0136): the tempo.edit elastic words, the
# warp-marker model and the Warp panel. A module of its own so ui/qt's CMake
# is untouched: its Qt-free sources join sonora_ui_edit, where edit_tempo.cpp
# (which dispatches the `elastic` word) lives, as ui/groove does. Nothing
# here exists where ui/qt was skipped (no Qt 6).
if(NOT TARGET sonora_ui_edit)
  return()
endif()

target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_elastic/elastic_command.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_elastic/warp_markers.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/elastic_command.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_markers.cpp
)
# PUBLIC: edit_tempo.cpp finds elastic_command.h through it (__has_include).
# sonora_api for api/elastic.h, the request's grammar.
target_include_directories(sonora_ui_edit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sonora_ui_edit PUBLIC sonora_api)

# The panel's QML as text in a generated header, for the reason ui/groove
# gives: a Qt resource from here on a target ui/qt defines generated nothing.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/WarpPanel.qml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/qml/WarpPanel.qml SONORA_WARP_PANEL_QML)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/warp_panel_qml.inc
  CONTENT "R\"sonoraqml(${SONORA_WARP_PANEL_QML})sonoraqml\"\n" @ONLY)
target_sources(sonora_ui_edit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/sonora/ui_elastic/warp_panel.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_panel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/qml/WarpPanel.qml
)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/qml/WarpPanel.qml
  PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(sonora_ui_edit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# One test file per area: the words against the fake engine, the marker model.
add_executable(test_ui_elastic tests/test_elastic_words.cpp)
target_link_libraries(test_ui_elastic PRIVATE sonora_ui_edit sonora_api_fake)
add_test(NAME ui_elastic COMMAND test_ui_elastic)
add_executable(test_ui_elastic_markers tests/test_warp_markers.cpp)
target_link_libraries(test_ui_elastic_markers PRIVATE sonora_ui_edit)
add_test(NAME ui_elastic_markers COMMAND test_ui_elastic_markers)
set_tests_properties(ui_elastic ui_elastic_markers PROPERTIES LABELS ui)
