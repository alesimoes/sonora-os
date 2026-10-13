# The audio backend seam: AudioBackend, ProcessCallback, AudioBlock. Header
# only. engine/core implements ProcessCallback; drivers/* implement
# AudioBackend; neither links the other.
add_library(sonora_backend_api INTERFACE)
target_include_directories(sonora_backend_api INTERFACE include)
target_link_libraries(sonora_backend_api INTERFACE sonora_base)

add_executable(test_select_device tests/test_select_device.cpp)
target_link_libraries(test_select_device PRIVATE sonora_backend_api)
add_test(NAME select_device COMMAND test_select_device)

add_subdirectory(conformance)
