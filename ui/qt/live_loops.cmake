# The Live Loops grid (ADR-0164): its model over launch::Launcher, its QML
# and its test, hooked in here so the shared lists stay untouched. Included
# by CMakeLists.txt after sonora_ui_qml exists.
if(NOT TARGET sonora_launch AND EXISTS "${CMAKE_SOURCE_DIR}/common/launch/CMakeLists.txt")
  add_subdirectory("${CMAKE_SOURCE_DIR}/common/launch" "${CMAKE_BINARY_DIR}/common/launch")
endif()
if(TARGET sonora_launch)
  sonora_ui_library(sonora_ui_live_loops QT SOURCES
    src/live_loops_model.h src/live_loops_model.cpp qml/LiveLoops.qml)
  set_source_files_properties(qml/LiveLoops.qml PROPERTIES HEADER_FILE_ONLY ON)
  target_link_libraries(sonora_ui_live_loops PUBLIC sonora_launch)
  target_link_libraries(sonora_ui_qml PUBLIC sonora_ui_live_loops)
  qt_add_resources(sonora_ui_qml sonora_qml_live_loops PREFIX /sonora FILES qml/LiveLoops.qml)

  add_executable(test_ui_live_loops tests/test_live_loops.cpp)
  target_link_libraries(test_ui_live_loops PRIVATE sonora_ui_qt Qt6::Test)
  add_test(NAME ui_live_loops COMMAND test_ui_live_loops)
  set_tests_properties(ui_live_loops PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
