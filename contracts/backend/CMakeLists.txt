# The audio backend seam: AudioBackend, ProcessCallback, AudioBlock. Header
# only. engine/core implements ProcessCallback; drivers/* implement
# AudioBackend; neither links the other.
add_library(sonora_backend_api INTERFACE)
target_include_directories(sonora_backend_api INTERFACE include)
target_link_libraries(sonora_backend_api INTERFACE sonora_base)
