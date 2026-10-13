# The backend conformance suite (ADR-0007 §7). Test code, not part of the
# contract's interface: a static library each backend's test links with a
# Rig of its own. drivers/fake runs it in ctest; drivers/jack builds
# sonora-conformance-jack, which tools/test-engine-docker.sh runs against
# PipeWire. Platform-neutral, like the contract (ADR-0008).
add_library(sonora_backend_conformance STATIC src/backend_conformance.cpp)
target_include_directories(sonora_backend_conformance PUBLIC include)
target_link_libraries(sonora_backend_conformance PUBLIC sonora_backend_api)
