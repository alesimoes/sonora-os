# Offline elastic audio (ADR-0136): the warp map, the quantize planner and the
# Stretcher interface with a deterministic fake. Pure, off the audio thread; no
# stretching library here (the adapter lives in engine/elastic).
add_library(sonora_stretch STATIC src/warp_map.cpp src/quantize.cpp src/fake_stretcher.cpp)
target_include_directories(sonora_stretch PUBLIC include)
target_link_libraries(sonora_stretch PUBLIC sonora_model_audited)

add_executable(stretch_warp_test tests/test_warp_map.cpp)
target_link_libraries(stretch_warp_test PRIVATE sonora_stretch)
add_test(NAME stretch_warp COMMAND stretch_warp_test)

add_executable(stretch_quantize_test tests/test_quantize.cpp)
target_link_libraries(stretch_quantize_test PRIVATE sonora_stretch)
add_test(NAME stretch_quantize COMMAND stretch_quantize_test)
