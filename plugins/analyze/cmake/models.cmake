# Manifest loader, file digest and the ONNX runner (ADR-0066 §3, §2). Included
# by plugins/analyze/CMakeLists.txt; SONORA_ONNX_FOUND is set there.
target_sources(sonora_analyze PRIVATE src/models/model_loader.cpp)
target_include_directories(sonora_analyze PUBLIC src/models)
target_link_libraries(sonora_analyze PRIVATE sonora_sha256)
if(SONORA_ONNX_FOUND)
  target_sources(sonora_analyze PRIVATE src/models/runner_ort.cpp)
  target_include_directories(sonora_analyze PRIVATE ${SONORA_ONNX_INCLUDE_DIR})
  target_link_libraries(sonora_analyze PRIVATE ${SONORA_ONNX_LIBRARY})
else()
  target_sources(sonora_analyze PRIVATE src/models/runner_stub.cpp)
endif()

add_executable(test_analyze_models tests/test_models.cpp)
target_link_libraries(test_analyze_models PRIVATE sonora_analyze sonora_sha256)
target_compile_definitions(test_analyze_models PRIVATE
  SONORA_MANIFEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../native/models/manifest.json")
if(NOT SONORA_ONNX_FOUND)
  target_compile_definitions(test_analyze_models PRIVATE SONORA_TEST_EXPECT_NO_ONNX=1)
endif()
add_test(NAME analyze_models COMMAND test_analyze_models)
