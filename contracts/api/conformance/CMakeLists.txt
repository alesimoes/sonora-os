# The control API's conformance suite (ADR-0007 §7). A library, as the
# backend suite is: contracts/api's ctest runs it on FakeEngine, and
# engine/control's on InProcessEngine over drivers/fake. It links
# common/model to check the model path with a real replica.
add_library(sonora_api_conformance STATIC src/conformance.cpp)
target_include_directories(sonora_api_conformance PUBLIC include)
target_link_libraries(sonora_api_conformance PUBLIC sonora_api PRIVATE sonora_model)
# Over 10 min under GCC's ASan+UBSan with the alignment check, 3 min without:
# it builds, copies and encodes compounds of the model's 62-way variants (see
# common/model/CMakeLists.txt).
if(SONORA_GCC_UBSAN_ALIGNMENT)
  target_compile_options(sonora_api_conformance PRIVATE -fno-sanitize=alignment)
endif()
