# Loop catalog (ADR-0163): interfaces and fakes are header-only; the scanner,
# metadata parsers and filter are a small static library. No dependencies.
add_library(sonora_loops INTERFACE)
target_include_directories(sonora_loops INTERFACE include)

add_library(sonora_loops_scan STATIC
    src/loop_filter.cpp src/loop_meta.cpp src/loop_scan.cpp)
target_link_libraries(sonora_loops_scan PUBLIC sonora_loops)

add_executable(loops_contract_test tests/test_loops_contract.cpp)
target_link_libraries(loops_contract_test PRIVATE sonora_loops)
add_test(NAME loops_contract COMMAND loops_contract_test)

add_executable(loops_scan_test tests/test_loops_scan.cpp)
target_link_libraries(loops_scan_test PRIVATE sonora_loops_scan)
add_test(NAME loops_scan COMMAND loops_scan_test)

add_executable(loops_filter_test tests/test_loops_filter.cpp)
target_link_libraries(loops_filter_test PRIVATE sonora_loops_scan)
add_test(NAME loops_filter COMMAND loops_filter_test)
