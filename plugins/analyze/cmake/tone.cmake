# The tone-match pass (ADR-0072): included by plugins/analyze/CMakeLists.txt.
# The matcher reuses the timbre pass's EQ fit (match_timbre), so timbre.cmake
# must come first in the include order there.
target_sources(sonora_analyze PRIVATE src/tone/tone_matcher.cpp)
target_include_directories(sonora_analyze PUBLIC src/tone)

# The command and its tests are other slices'; they appear when present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tone_cmd.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tone_cmd.cmake")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tone_tests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tone_tests.cmake")
endif()
