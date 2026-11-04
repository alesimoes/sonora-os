# What the native editors need of the processors (ADR-0057, native.editors):
# the hints each declares are coherent with its parameter table, and each
# publishes the meters and the curve it declares, from process(), without
# allocating. Not a processor: plugins/native/CMakeLists.txt skips this
# directory when it collects them.
add_executable(test_native_editors tests/test_editors.cpp)
target_link_libraries(test_native_editors PRIVATE
  sonora_native_compressor sonora_native_fet sonora_native_opto sonora_native_limiter sonora_native_eq)
add_test(NAME native_editors COMMAND test_native_editors)
