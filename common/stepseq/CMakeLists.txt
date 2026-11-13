# Step sequencer contract (ADR-0169): pattern model, PatternPlayer interface,
# the lock-free snapshot exchange and a reference fake player. Platform-neutral.
# Hooked from common/stretch/CMakeLists.txt until the root file takes it.
add_library(sonora_stepseq STATIC src/fake_player.cpp src/pattern_player.cpp src/step_timing.cpp)
target_include_directories(sonora_stepseq PRIVATE src)
target_include_directories(sonora_stepseq PUBLIC include)
target_compile_features(sonora_stepseq PUBLIC cxx_std_17)

add_executable(stepseq_contract_test tests/test_contract.cpp)
target_link_libraries(stepseq_contract_test PRIVATE sonora_stepseq)
add_test(NAME stepseq_contract COMMAND stepseq_contract_test)
set_tests_properties(stepseq_contract PROPERTIES LABELS stepseq)

foreach(t timing swing probability rows block_size exchange_wiring)
    add_executable(stepseq_${t}_test tests/test_${t}.cpp)
    target_link_libraries(stepseq_${t}_test PRIVATE sonora_stepseq)
    target_include_directories(stepseq_${t}_test PRIVATE tests)
    add_test(NAME stepseq_${t} COMMAND stepseq_${t}_test)
    set_tests_properties(stepseq_${t} PROPERTIES LABELS stepseq)
endforeach()
