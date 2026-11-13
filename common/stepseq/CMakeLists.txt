# Step sequencer contract (ADR-0169): pattern model, PatternPlayer interface,
# the lock-free snapshot exchange and a reference fake player. Platform-neutral.
# Hooked from common/stretch/CMakeLists.txt until the root file takes it.
add_library(sonora_stepseq STATIC src/fake_player.cpp)
target_include_directories(sonora_stepseq PUBLIC include)
target_compile_features(sonora_stepseq PUBLIC cxx_std_17)

add_executable(stepseq_contract_test tests/test_contract.cpp)
target_link_libraries(stepseq_contract_test PRIVATE sonora_stepseq)
add_test(NAME stepseq_contract COMMAND stepseq_contract_test)
set_tests_properties(stepseq_contract PROPERTIES LABELS stepseq)
