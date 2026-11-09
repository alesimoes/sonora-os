# The LSTM kernel (ADR-0111), included by common/dsp/CMakeLists.txt so this
# slice never edits that file. Paths are relative to common/dsp.
#
# src/neural/lstm.cpp is baseline code. Its AVX2 product
# (src/arch/x86_64/lstm_avx2.cpp) is compiled for AVX2 without FMA, the
# options of neural_avx2.cpp, so it gives the generic product's bits, and is
# called only after CPUID has found AVX2 (neural_simd.cpp).
target_sources(sonora_dsp PRIVATE src/neural/lstm.cpp)
if(SONORA_DSP_ARCH STREQUAL "x86_64")
  target_sources(sonora_dsp PRIVATE src/arch/x86_64/lstm_avx2.cpp)
  if(MSVC)
    set_source_files_properties(src/arch/x86_64/lstm_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(src/arch/x86_64/lstm_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mno-fma")
  endif()
endif()

# Behaviour: reset determinism, block-size independence, silence, non-finite
# and huge input, and the generic and AVX2 products to the same bits.
add_executable(test_dsp_lstm tests/test_lstm.cpp)
target_link_libraries(test_dsp_lstm PRIVATE sonora_dsp)
target_compile_definitions(test_dsp_lstm PRIVATE SONORA_SANITIZED=$<BOOL:${SONORA_SANITIZED}>)
add_test(NAME dsp_lstm COMMAND test_dsp_lstm)
