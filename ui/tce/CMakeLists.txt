# Time compression/expansion dialog over elastic ratio (ADR-0175). Its own
# module and CMake; reuses ui/elastic's command path, never edits it.
if(NOT TARGET sonora_ui_edit)
  return()
endif()

add_library(sonora_ui_tce STATIC src/tce_ratio.cpp src/tce_dialog.cpp)
target_include_directories(sonora_ui_tce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sonora_ui_tce PUBLIC sonora_ui_edit sonora_api)

add_executable(test_ui_tce_ratio tests/test_tce_ratio.cpp)
target_link_libraries(test_ui_tce_ratio PRIVATE sonora_ui_tce)
add_test(NAME ui_tce_ratio COMMAND test_ui_tce_ratio)
add_executable(test_ui_tce_dialog tests/test_tce_dialog.cpp)
target_link_libraries(test_ui_tce_dialog PRIVATE sonora_ui_tce sonora_api_fake)
add_test(NAME ui_tce_dialog COMMAND test_ui_tce_dialog)
set_tests_properties(ui_tce_ratio ui_tce_dialog PROPERTIES LABELS ui)
