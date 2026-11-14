# Amp (ADR-0011 stage N3 section 4.3, ADR-0111 sections 5-6): a NAM-format
# capture, WaveNet or LSTM (common/dsp/neural), behind one NeuralModel
# interface, resampled when the session is not at the capture's rate,
# timed against class E at load and normalised from its metadata. The
# capture is a file slot ("model", .nam) read by ampcab's reader, which is
# neutral C++ (sonora_json), so the slot exists in every build.
add_library(sonora_native_amp STATIC src/amp.cpp src/loader.cpp src/chain.cpp src/model.cpp)
target_include_directories(sonora_native_amp PUBLIC include)
target_link_libraries(sonora_native_amp PUBLIC sonora_native_sdk sonora_dsp PRIVATE sonora_native_ampcab_nam)

add_library(sonora_native_amp_suite STATIC tests/suite.cpp tests/suite_load.cpp)
target_link_libraries(sonora_native_amp_suite PUBLIC sonora_native_amp sonora_dsp_measure sonora_native_ampcab_nam)
target_compile_definitions(sonora_native_amp_suite PRIVATE
    SONORA_AMP_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../ampcab/tests/data")

sonora_native_quality_test(amp)


# The LV2 and VST3 bundles come with the bundle driver's .nam state
# scenario (the cabinet's writes a WAV); until then the processor is
# measured here and not installed.
