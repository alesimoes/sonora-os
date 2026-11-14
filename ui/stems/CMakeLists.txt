# Split into stems dialog model (logic.stem_split, ADR-0223): Qt-free, on
# common/stems. Hooked from the root.
find_package(Threads REQUIRED)
add_library(sonora_ui_stems STATIC src/stem_split_model.cpp)
target_include_directories(sonora_ui_stems PUBLIC include)
target_link_libraries(sonora_ui_stems PUBLIC sonora_stems Threads::Threads)
target_compile_features(sonora_ui_stems PUBLIC cxx_std_17)

add_executable(test_ui_stems_model tests/test_stems_model.cpp)
target_link_libraries(test_ui_stems_model PRIVATE sonora_ui_stems)
add_test(NAME ui_stems_model COMMAND test_ui_stems_model)
set_tests_properties(ui_stems_model PROPERTIES LABELS ui)
