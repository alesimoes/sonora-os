# logic.live_loops (ADR-0224): the launcher integration contract's test.
# Included by the deferred hook in common/launch/CMakeLists.txt, in the root's
# scope, so every path is absolute. The contract is header-only: the test
# needs engine/control's include directory and sonora_launch, not the whole
# of sonora_control. Moves into engine/control/CMakeLists.txt when that file
# frees; the root's directory labels do not see it, so it sets "control".
add_executable(launch_controller_contract_test
  "${CMAKE_SOURCE_DIR}/engine/control/tests/launch_controller_contract_test.cpp")
target_include_directories(launch_controller_contract_test PRIVATE
  "${CMAKE_SOURCE_DIR}/engine/control/include")
target_link_libraries(launch_controller_contract_test PRIVATE sonora_launch)
add_test(NAME launch_controller_contract COMMAND launch_controller_contract_test)
set_tests_properties(launch_controller_contract PROPERTIES LABELS "control;launch")
