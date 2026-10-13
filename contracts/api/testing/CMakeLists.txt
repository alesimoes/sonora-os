# FakeEngine (ADR-0007 §7): the provider's fake, for interface tests. Links
# common/model so that the model path applies operations with the real
# apply(); a fake with its own would be a second implementation to drift.
add_library(sonora_api_fake STATIC src/fake_engine.cpp)
target_include_directories(sonora_api_fake PUBLIC include)
target_link_libraries(sonora_api_fake PUBLIC sonora_api PRIVATE sonora_model)
