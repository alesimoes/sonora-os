# .ptx import (ADR-0156): neutral document, reader interface, importer.
# Reached from interchange/CMakeLists.txt by add_subdirectory(ptx) once
# interchange.sessions.aaf_import has merged. Until then configure it
# standalone, with a sonora_interchange target defined by the caller.
add_library(sonora_ptx INTERFACE)
target_include_directories(sonora_ptx INTERFACE include)
target_link_libraries(sonora_ptx INTERFACE sonora_interchange)

add_executable(test_ptx_contract tests/test_ptx_contract.cpp)
target_include_directories(test_ptx_contract PRIVATE tests)
target_link_libraries(test_ptx_contract PRIVATE sonora_ptx)
add_test(NAME interchange_ptx_contract COMMAND test_ptx_contract)
