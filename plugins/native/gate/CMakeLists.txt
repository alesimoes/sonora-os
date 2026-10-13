# Gate and expander (ADR-0011 N0).
add_library(sonora_native_gate STATIC src/gate.cpp)
target_include_directories(sonora_native_gate PUBLIC include)
target_link_libraries(sonora_native_gate PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_gate_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_gate_suite PUBLIC sonora_native_gate sonora_dsp_measure)

sonora_native_quality_test(gate)

# The LV2 and VST3 bundles (ADR-0011 N1). No side-chain input yet: the
# host does not route one (brain/research/native-format-n1.md).
sonora_native_plugin(gate FACTORY make_gate DISPLAY "Sonora Gate" UID SNgt MONO_UID SMgt
  LV2_CATEGORY "lv2:GatePlugin" VST3_CATEGORIES "Fx|Dynamics" SIDECHAIN)
