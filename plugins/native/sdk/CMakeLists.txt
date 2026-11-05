# The native Processor interface, the parameter table and its descriptor,
# state and preset codecs (ADR-0011 §1.2, §2.4). Neutral: standard C++17
# (ADR-0008). Edges: common/base, common/json (§2.2), and common/dsp for the
# flush-to-zero guard and the smoothers the base class shares with every
# processor; that edge is not in §2.2's table and is proposed in this PR.
add_library(sonora_native_sdk STATIC
  src/param.cpp
  src/processor_base.cpp
  src/state.cpp
)
target_include_directories(sonora_native_sdk PUBLIC include)
target_link_libraries(sonora_native_sdk PUBLIC sonora_base sonora_dsp PRIVATE sonora_json)
# telemetry.h includes contracts/plugin's header-only region layout (ADR-0057).
# By include path, not by the sonora_plugin_protocol target: the neutral build
# (Portable MSVC) does not configure contracts/, and the header needs only
# the standard library and common/base's rt.h.
target_include_directories(sonora_native_sdk PUBLIC ${PROJECT_SOURCE_DIR}/contracts/plugin/include)

add_executable(test_native_sdk tests/test_sdk.cpp)
target_link_libraries(test_native_sdk PRIVATE sonora_native_sdk)
add_test(NAME native_sdk COMMAND test_native_sdk)

# The telemetry publisher (ADR-0057): header-only over the region's layout.
add_executable(test_native_telemetry tests/test_telemetry.cpp)
target_link_libraries(test_native_telemetry PRIVATE sonora_native_sdk)
add_test(NAME native_telemetry COMMAND test_native_telemetry)
