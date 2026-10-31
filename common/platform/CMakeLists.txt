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
  include/sonora/platform/file.h
  include/sonora/platform/wake.h
  src/${SONORA_PLATFORM_IMPL}/thread_cpu.cpp
  src/${SONORA_PLATFORM_IMPL}/ipc.cpp
  src/${SONORA_PLATFORM_IMPL}/process.cpp
  src/${SONORA_PLATFORM_IMPL}/file.cpp
  src/${SONORA_PLATFORM_IMPL}/wake.cpp
)
target_include_directories(sonora_platform PUBLIC include)
# sonora_base for SONORA_RT, which wake.h's wake_all() carries (ADR-0012, A3).
target_link_libraries(sonora_platform PUBLIC sonora_base Threads::Threads)

add_executable(test_thread_cpu tests/test_thread_cpu.cpp)
target_link_libraries(test_thread_cpu PRIVATE sonora_platform Threads::Threads)
add_test(NAME platform_thread_cpu COMMAND test_thread_cpu)

# The control channel and the read-only region (ADR-0007 §3): POSIX only,
# because the test also checks what the kernel refuses a client.
if(UNIX)
  add_executable(test_platform_ipc tests/test_ipc.cpp)
  target_link_libraries(test_platform_ipc PRIVATE sonora_platform Threads::Threads)
  add_test(NAME platform_ipc COMMAND test_platform_ipc)

  # The unsupported stubs claim no durability, so this is POSIX's test.
  add_executable(test_platform_file_sync tests/test_file_sync.cpp)
  target_link_libraries(test_platform_file_sync PRIVATE sonora_platform)
  add_test(NAME platform_file_sync COMMAND test_platform_file_sync)

  # The plugin scanner's seam (ADR-0012, amendment of 2026-10-19): the
  # cache folder, a status through a symlink with its change time, and a
  # background thread and child at nice 10 and the idle I/O class.
  add_executable(test_platform_background tests/test_background.cpp)
  target_link_libraries(test_platform_background PRIVATE sonora_platform Threads::Threads)
  add_test(NAME platform_background COMMAND test_platform_background)

  # ChildProcess::exit_handle(), the pidfd plugin hosts are watched by
  # (ADR-0012 §4). Linux only: elsewhere POSIX has no pidfd and it is -1.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_platform_child_exit tests/test_child_exit.cpp)
    target_link_libraries(test_platform_child_exit PRIVATE sonora_platform)
    add_test(NAME platform_child_exit COMMAND test_platform_child_exit)
    set_tests_properties(platform_child_exit PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  # The unsupported stub claims no exclusion; the other holder is a fork.
  add_executable(test_platform_folder_lock tests/test_folder_lock.cpp)
  target_link_libraries(test_platform_folder_lock PRIVATE sonora_platform)
  add_test(NAME platform_folder_lock COMMAND test_platform_folder_lock)

  # The medium checks (ADR-0018 §8): free bytes and a read-only folder,
  # checked against statvfs and a folder without write permission.
  add_executable(test_platform_medium tests/test_medium.cpp)
  target_link_libraries(test_platform_medium PRIVATE sonora_platform)
  add_test(NAME platform_medium COMMAND test_platform_medium)
endif()
