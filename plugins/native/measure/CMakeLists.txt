# The measurement harness (ADR-0011 §3.1): a library the processor suites
# use, and the sonora-dsp-measure executable (app/) that runs them.
# Edges: common/dsp, the SDK (§2.2), and common/json to read quality.json
# and write reports; that one is not in §2.2's table and is proposed in
# this PR. Golden files are 32-bit float WAVs read and written here, so
# common/audiofile (a BWF take writer) is not needed.
add_library(sonora_dsp_measure STATIC
  src/analysis.cpp
  src/calibrate.cpp
  src/checks.cpp
  src/render.cpp
  src/signals.cpp
  src/spice.cpp
  src/suite.cpp
  src/wav.cpp
)
target_include_directories(sonora_dsp_measure PUBLIC include)
target_link_libraries(sonora_dsp_measure PUBLIC sonora_native_sdk sonora_dsp sonora_json)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/dsp-reports)

# One ctest per processor: its suite against its quality.json and golden
# files, writing build/dsp-reports/<name>.json. CPU is not measured here:
# shared CI runners say nothing about the reference machine (§3.5).
function(sonora_native_quality_test name)
  add_test(NAME native_${name}
    COMMAND sonora-dsp-measure --processor ${name}
            --quality ${CMAKE_CURRENT_SOURCE_DIR}/quality.json
            --golden ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
            --report ${CMAKE_BINARY_DIR}/dsp-reports)
endfunction()
