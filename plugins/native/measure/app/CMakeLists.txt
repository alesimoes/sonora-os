# sonora-dsp-measure links every processor's suite: a wiring executable at
# the top of the native tree, as engine/app is for the engine.
add_executable(sonora-dsp-measure main.cpp host_fixture.cpp)
target_link_libraries(sonora-dsp-measure PRIVATE
  sonora_native_eq_suite
  sonora_native_compressor_suite
  sonora_native_gate_suite
  sonora_native_limiter_suite
  sonora_native_delay_suite
  sonora_native_chorus_suite
  sonora_native_reverb_suite
)
