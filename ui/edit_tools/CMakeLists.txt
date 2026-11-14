# Qt-free view-tool logic (ADR-0158). Guarded so it is added once whichever
# CMakeLists reaches it first.
if(NOT TARGET sonora_ui_edit_tools)
  add_library(sonora_ui_edit_tools STATIC src/edit_tools.cpp)
  target_include_directories(sonora_ui_edit_tools PUBLIC include)

  add_executable(test_ui_edit_tools tests/test_edit_tools.cpp)
  target_link_libraries(test_ui_edit_tools PRIVATE sonora_ui_edit_tools)
  add_test(NAME test_ui_edit_tools COMMAND test_ui_edit_tools)
  set_tests_properties(test_ui_edit_tools PROPERTIES LABELS ui)
endif()
