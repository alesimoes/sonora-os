# The OS seam (ADR-0008, seam 1). Neutral headers; one implementation per
# platform, chosen here by source file and never by #ifdef in a caller.
# Layer 0 beside common/base: any module may link it except common/base and
# contracts/*, and a module links it only when it needs an OS service.
#
# Until ADR-0008 P6 brings SONORA_PLATFORM, UNIX picks posix and anything
# else gets the unsupported stubs, which report "unavailable" loudly.
if(UNIX)
  set(SONORA_PLATFORM_IMPL posix)
else()
  set(SONORA_PLATFORM_IMPL unsupported)
endif()

add_library(sonora_platform STATIC
  include/sonora/platform/thread_cpu.h
  src/${SONORA_PLATFORM_IMPL}/thread_cpu.cpp
)
target_include_directories(sonora_platform PUBLIC include)

add_executable(test_thread_cpu tests/test_thread_cpu.cpp)
target_link_libraries(test_thread_cpu PRIVATE sonora_platform Threads::Threads)
add_test(NAME platform_thread_cpu COMMAND test_thread_cpu)
