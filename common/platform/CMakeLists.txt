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
  include/sonora/platform/ipc.h
  include/sonora/platform/process.h
  src/${SONORA_PLATFORM_IMPL}/thread_cpu.cpp
  src/${SONORA_PLATFORM_IMPL}/ipc.cpp
  src/${SONORA_PLATFORM_IMPL}/process.cpp
)
target_include_directories(sonora_platform PUBLIC include)
target_link_libraries(sonora_platform PUBLIC Threads::Threads)

add_executable(test_thread_cpu tests/test_thread_cpu.cpp)
target_link_libraries(test_thread_cpu PRIVATE sonora_platform Threads::Threads)
add_test(NAME platform_thread_cpu COMMAND test_thread_cpu)

# The control channel and the read-only region (ADR-0007 §3): POSIX only,
# because the test also checks what the kernel refuses a client.
if(UNIX)
  add_executable(test_platform_ipc tests/test_ipc.cpp)
  target_link_libraries(test_platform_ipc PRIVATE sonora_platform Threads::Threads)
  add_test(NAME platform_ipc COMMAND test_platform_ipc)
endif()
