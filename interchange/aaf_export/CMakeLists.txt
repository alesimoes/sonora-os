# AAF export (ADR-0161). Not yet reached from interchange/CMakeLists.txt (a
# follow-up once aaf_import merges); build it with the root project and
# -DCMAKE_PROJECT_INCLUDE=<file adding this directory>.
add_library(sonora_aaf_export STATIC src/aaf_export.cpp)
target_include_directories(sonora_aaf_export PUBLIC include)
target_link_libraries(sonora_aaf_export PUBLIC sonora_interchange)

add_executable(test_aaf_export_contract tests/test_aaf_export_contract.cpp)
target_link_libraries(test_aaf_export_contract PRIVATE sonora_aaf_export)
target_include_directories(test_aaf_export_contract PRIVATE tests)
add_test(NAME aaf_export_contract COMMAND test_aaf_export_contract)
set_tests_properties(aaf_export_contract PROPERTIES LABELS interchange)
