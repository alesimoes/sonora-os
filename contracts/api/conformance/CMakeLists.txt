# The control API's conformance suite (ADR-0007 §7). A library, as the
# backend suite is: contracts/api's ctest runs it on FakeEngine, and
# engine/control's on InProcessEngine over drivers/fake. It links
# common/model to check the model path with a real replica.
add_library(sonora_api_conformance STATIC src/conformance.cpp)
target_include_directories(sonora_api_conformance PUBLIC include)
target_link_libraries(sonora_api_conformance PUBLIC sonora_api PRIVATE sonora_model)
