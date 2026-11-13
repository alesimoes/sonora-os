# Loop catalog interfaces and fakes (ADR-0163). Header-only, no dependencies.
add_library(sonora_loops INTERFACE)
target_include_directories(sonora_loops INTERFACE include)

add_executable(loops_contract_test tests/test_loops_contract.cpp)
target_link_libraries(loops_contract_test PRIVATE sonora_loops)
add_test(NAME loops_contract COMMAND loops_contract_test)
