# The Pitch panel (time.pitch, ADR-0143): PitchPanel.qml as text in a
# generated header, as ui/groove ships its panel. No Qt is compiled here (only
# ui/qt may include it, tools/check-deps.sh); the screenshot scene that shows
# the panel is ui/qt/src/scenes/pitch_editor.cpp, and it is built only where
# sonora-ui is, because this library is linked into it once the root has been
# read (ui/qt is added after plugins/analyze, which adds this directory).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/PitchPanel.qml)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/PitchPanel.qml SONORA_PITCH_PANEL_QML)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/pitch_panel_qml.inc
  CONTENT "R\"sonoraqml(${SONORA_PITCH_PANEL_QML})sonoraqml\"\n" @ONLY)

add_library(sonora_ui_pitch_panel STATIC
  include/sonora/ui_pitch/pitch_panel.h
  src/pitch_panel.cpp
  PitchPanel.qml PitchRoll.qml PitchInspector.qml PitchScaleBar.qml)
set_source_files_properties(PitchPanel.qml PitchRoll.qml PitchInspector.qml PitchScaleBar.qml PROPERTIES HEADER_FILE_ONLY ON)
target_include_directories(sonora_ui_pitch_panel PUBLIC include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sonora_ui_pitch_panel PUBLIC sonora_ui_pitch)

if(CMAKE_VERSION VERSION_LESS 3.19)
  message(WARNING "ui/pitch/qt needs CMake 3.19 (cmake_language DEFER); the Pitch panel is not linked")
else()
  function(_sonora_pitch_join_ui)
    if(TARGET sonora-ui)
      target_link_libraries(sonora-ui PRIVATE sonora_ui_pitch_panel)
    endif()
  endfunction()
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _sonora_pitch_join_ui)
endif()
