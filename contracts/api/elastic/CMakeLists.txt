# audio.elastic's contract tests (ADR-0136): the argument's grammar and
# refusals (api/elastic.h, header-only in contracts/api/include), the
# capability gating and FakeEngine's handling. A directory of its own so
# contracts/api/CMakeLists.txt is not edited; the root adds it after
# contracts/api, whose targets it links.
if(NOT TARGET sonora_api_fake)
  return()
endif()

add_executable(test_api_elastic tests/test_elastic.cpp)
target_link_libraries(test_api_elastic PRIVATE sonora_api sonora_api_fake sonora_model)
add_test(NAME api_elastic COMMAND test_api_elastic)
set_tests_properties(api_elastic PROPERTIES LABELS contract)
