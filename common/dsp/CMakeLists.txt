# Neutral DSP primitives for the native plugin suite (ADR-0011 §2.1), layer 1:
# links common/base only. Standard C++17, no I/O, no threads (ADR-0011 §2.2,
# ADR-0008). Architecture-specific code is chosen here from the target
# processor, never by #ifdef (ADR-0011 §2.3); generic/ always exists.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  set(SONORA_DSP_ARCH x86_64)
else()
  set(SONORA_DSP_ARCH generic)
endif()
# The floating-point control (denormals.cpp) is chosen on its own: AArch64
# has FPCR.FZ but no vector kernels of its own yet, so it takes the generic
# kernels and its own flush-to-zero. The aarch64 file reads FPCR with GCC/
# Clang inline assembly; MSVC on ARM64 (no target today) keeps generic.
if(SONORA_DSP_ARCH STREQUAL "x86_64")
  set(SONORA_DSP_FP_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND
       CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  set(SONORA_DSP_FP_ARCH aarch64)
else()
  set(SONORA_DSP_FP_ARCH generic)
endif()
message(STATUS "common/dsp: architecture code from src/arch/${SONORA_DSP_ARCH}, "
               "flush-to-zero from src/arch/${SONORA_DSP_FP_ARCH}")

add_library(sonora_dsp STATIC
  src/biquad.cpp
  src/convolver.cpp
  src/fdn.cpp
  src/fet.cpp
  src/fft.cpp
  src/limiter.cpp
  src/loudness.cpp
  src/meter.cpp
  src/neural/kernels.cpp
  src/neural/lstm_spec.cpp
  src/neural/wavenet.cpp
  src/noise.cpp
  src/opto.cpp
  src/oversampling.cpp
  src/reference_peak.cpp
  src/resample.cpp
  src/supply.cpp
  src/tape.cpp
  src/transformer.cpp
  src/triode.cpp
  src/true_peak.cpp
  src/arch/${SONORA_DSP_FP_ARCH}/denormals.cpp
  src/arch/${SONORA_DSP_ARCH}/convolver_kernels.cpp
  src/arch/${SONORA_DSP_ARCH}/tape_pair.cpp
  src/arch/${SONORA_DSP_ARCH}/triode_pair.cpp
  src/arch/${SONORA_DSP_ARCH}/transformer_pair.cpp
  src/arch/${SONORA_DSP_ARCH}/neural_simd.cpp
)
# The WaveNet's AVX2 kernels (src/arch/x86_64/neural_avx2.cpp): this one
# file is compiled for AVX2, without FMA, and is called only after CPUID
# has found AVX2 (neural_simd.cpp); everything else stays at the x86-64
# baseline, so the library still runs on a processor without it. CPUID is
# spelled per compiler family, so its file is chosen here too.
if(SONORA_DSP_ARCH STREQUAL "x86_64")
  if(MSVC)
    target_sources(sonora_dsp PRIVATE src/arch/x86_64/neural_avx2.cpp src/arch/x86_64/cpu_msvc.cpp)
    set_source_files_properties(src/arch/x86_64/neural_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    target_sources(sonora_dsp PRIVATE src/arch/x86_64/neural_avx2.cpp src/arch/x86_64/cpu_gnu.cpp)
    set_source_files_properties(src/arch/x86_64/neural_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mno-fma")
  endif()
endif()
# The LSTM kernel (src/neural/lstm.cpp, ADR-0111) adds itself here when it
# lands; until then the library carries the spec functions only and nothing
# may call Lstm's members.
include(cmake/lstm_kernel.cmake OPTIONAL)
target_include_directories(sonora_dsp PUBLIC include)
target_link_libraries(sonora_dsp PUBLIC sonora_base)
# No fused multiply-add contraction: golden files must not depend on what a
# compiler chooses to fuse (ADR-0011 §2.3, §3.4). PUBLIC so every processor
# built on these headers gets the same arithmetic. -ffast-math is never set.
target_compile_options(sonora_dsp PUBLIC
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
  $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)

# The WaveNet kernel's products are element-wise loops over a block's
# frames through pointers GCC cannot prove distinct; its -O2 "very cheap"
# vectoriser will not version a loop for aliasing, -O3 does. Element-wise,
# so vectorising reorders no sum: the output is the same bits either way.
set_source_files_properties(src/neural/wavenet.cpp src/neural/kernels.cpp PROPERTIES COMPILE_OPTIONS
  "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<CONFIG:Debug>>>:-O3>")

foreach(t filters convolver convolver_refusals resample oversampling loudness meter dynamics limiter fdn triode opto fet noise tape transformer neural lstm_contract)
  add_executable(test_dsp_${t} tests/test_${t}.cpp)
  target_link_libraries(test_dsp_${t} PRIVATE sonora_dsp)
  # The root CMakeLists.txt's SONORA_SANITIZED: timing bars are reported, not held.
  target_compile_definitions(test_dsp_${t} PRIVATE SONORA_SANITIZED=$<BOOL:${SONORA_SANITIZED}>)
  add_test(NAME dsp_${t} COMMAND test_dsp_${t})
endforeach()
# The portable two-lane triode solve (src/arch/generic/triode_pair.cpp)
# is what a non-x86 build ships; built here beside the x86-64 one, so
# test_triode's tick2-against-tick check runs on both whatever the host.
if(NOT SONORA_DSP_ARCH STREQUAL "generic")
  add_executable(test_dsp_triode_generic tests/test_triode.cpp src/triode.cpp src/oversampling.cpp src/fft.cpp
                 src/arch/generic/triode_pair.cpp src/arch/generic/denormals.cpp)
  target_include_directories(test_dsp_triode_generic PRIVATE include)
  target_compile_definitions(test_dsp_triode_generic PRIVATE SONORA_SANITIZED=$<BOOL:${SONORA_SANITIZED}>)
  target_link_libraries(test_dsp_triode_generic PRIVATE sonora_base)
  target_compile_options(test_dsp_triode_generic PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off> $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
  add_test(NAME dsp_triode_generic COMMAND test_dsp_triode_generic)
  # The same for the tape's two-channel integrator (tape_run_pair).
  add_executable(test_dsp_tape_generic tests/test_tape.cpp src/tape.cpp src/arch/generic/tape_pair.cpp)
  target_include_directories(test_dsp_tape_generic PRIVATE include)
  target_link_libraries(test_dsp_tape_generic PRIVATE sonora_base)
  target_compile_options(test_dsp_tape_generic PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off> $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
  add_test(NAME dsp_tape_generic COMMAND test_dsp_tape_generic)
  # And for the output stage's two-channel step (transformer_process_pair).
  # transformer.cpp flushes no denormals itself, so no denormals.cpp.
  add_executable(test_dsp_transformer_generic tests/test_transformer.cpp src/transformer.cpp
                 src/arch/generic/transformer_pair.cpp)
  target_include_directories(test_dsp_transformer_generic PRIVATE include)
  target_link_libraries(test_dsp_transformer_generic PRIVATE sonora_base)
  target_compile_options(test_dsp_transformer_generic PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off> $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
  add_test(NAME dsp_transformer_generic COMMAND test_dsp_transformer_generic)
  # And for the convolver's multiply-adds and head (convolver_kernels.cpp).
  add_executable(test_dsp_convolver_generic tests/test_convolver.cpp src/convolver.cpp src/fft.cpp src/noise.cpp
                 src/arch/generic/convolver_kernels.cpp)
  target_include_directories(test_dsp_convolver_generic PRIVATE include)
  target_compile_definitions(test_dsp_convolver_generic PRIVATE SONORA_SANITIZED=$<BOOL:${SONORA_SANITIZED}>)
  target_link_libraries(test_dsp_convolver_generic PRIVATE sonora_base)
  target_compile_options(test_dsp_convolver_generic PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off> $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
  add_test(NAME dsp_convolver_generic COMMAND test_dsp_convolver_generic)
endif()
# The slow ones (tens of seconds in Release, minutes under ASan) carry
# the "slow" label: PR CI runs them with SONORA_DSP_QUICK=1 (one rate, one
# setting), the nightly workflow runs `ctest -L slow` in full.
set_tests_properties(dsp_limiter dsp_loudness dsp_meter PROPERTIES LABELS slow)
# The tests that hold a wall-clock bar (a block's cost against another's, or
# against a period) run alone: under `ctest -j` the neighbours' load lands
# on one side of a ratio and not the other. The "timing" label lists them.
set(_dsp_timing_tests dsp_meter dsp_triode)
if(TEST dsp_triode_generic)
  list(APPEND _dsp_timing_tests dsp_triode_generic)
endif()
set_property(TEST ${_dsp_timing_tests} PROPERTY RUN_SERIAL TRUE)
set_property(TEST ${_dsp_timing_tests} APPEND PROPERTY LABELS timing)
