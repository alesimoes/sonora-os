# OMF import (ADR-0160): read-only, our own parser from the published spec,
# behind OmfReader, in the sonora-convert process only. Not yet reached from
# interchange/CMakeLists.txt (add_subdirectory(omf) follows aaf_import).
add_library(sonora_omf STATIC src/omf_import.cpp)
target_include_directories(sonora_omf PUBLIC include)
target_link_libraries(sonora_omf PUBLIC sonora_interchange)

add_executable(test_omf_contract tests/test_omf_contract.cpp)
target_link_libraries(test_omf_contract PRIVATE sonora_omf)
target_include_directories(test_omf_contract PRIVATE tests)
add_test(NAME omf_contract COMMAND test_omf_contract)
set_tests_properties(omf_contract PROPERTIES LABELS interchange)
