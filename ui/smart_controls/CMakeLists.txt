# Smart Controls (ADR-0117): the pure core. Hooked in from ui/client.
add_library(sonora_smart_controls STATIC src/smart_controls.cpp src/store.cpp)
target_include_directories(sonora_smart_controls PUBLIC include)
target_link_libraries(sonora_smart_controls PUBLIC sonora_json)

add_executable(test_smart_controls tests/test_smart_controls.cpp)
target_link_libraries(test_smart_controls PRIVATE sonora_smart_controls)
add_test(NAME smart_controls COMMAND test_smart_controls)
set_tests_properties(smart_controls PROPERTIES LABELS ui)

# The Qt part is added after ui/qt defines sonora_ui_session, so it is
# deferred to the end of the top-level configure.
function(_sonora_smart_controls_qt)
  if(TARGET sonora_ui_session AND
     EXISTS ${CMAKE_SOURCE_DIR}/ui/smart_controls/qt/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/ui/smart_controls/qt
                     ${CMAKE_BINARY_DIR}/ui/smart_controls/qt)
  endif()
endfunction()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _sonora_smart_controls_qt)
endif()
