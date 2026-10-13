# Neutral DSP primitives for the native plugin suite (ADR-0011 §2.1), layer 1:
# links common/base only. Standard C++17, no I/O, no threads (ADR-0011 §2.2,
# ADR-0008). Architecture-specific code is chosen here from the target
# processor, never by #ifdef (ADR-0011 §2.3); generic/ always exists.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  set(SONORA_DSP_ARCH x86_64)
else()
  set(SONORA_DSP_ARCH generic)
endif()
message(STATUS "common/dsp: architecture code from src/arch/${SONORA_DSP_ARCH}")

add_library(sonora_dsp STATIC
  src/biquad.cpp
  src/fft.cpp
  src/limiter.cpp
  src/loudness.cpp
  src/oversampling.cpp
  src/reference_peak.cpp
  src/true_peak.cpp
  src/arch/${SONORA_DSP_ARCH}/denormals.cpp
)
target_include_directories(sonora_dsp PUBLIC include)
target_link_libraries(sonora_dsp PUBLIC sonora_base)
# No fused multiply-add contraction: golden files must not depend on what a
# compiler chooses to fuse (ADR-0011 §2.3, §3.4). PUBLIC so every processor
# built on these headers gets the same arithmetic. -ffast-math is never set.
target_compile_options(sonora_dsp PUBLIC
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
  $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)

foreach(t filters oversampling loudness dynamics limiter)
  add_executable(test_dsp_${t} tests/test_${t}.cpp)
  target_link_libraries(test_dsp_${t} PRIVATE sonora_dsp)
  add_test(NAME dsp_${t} COMMAND test_dsp_${t})
endforeach()
# The two slow ones (tens of seconds in Release, minutes under ASan) carry
# the "slow" label: PR CI runs them with SONORA_DSP_QUICK=1 (one rate, one
# setting), the nightly workflow runs `ctest -L slow` in full.
set_tests_properties(dsp_limiter dsp_loudness PROPERTIES LABELS slow)
