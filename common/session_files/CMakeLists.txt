# A session folder on disk (ADR-0015 §3, §6; ADR-0007 update of
# 2026-10-25). Layer 2: the model's bytes, the platform's syncs. The
# engine's saver thread links it; the interface's tests link it to stand in
# for the engine, and the interface itself does not. Never on the audio
# thread.
add_library(sonora_session_files STATIC src/session_folder.cpp)
target_include_directories(sonora_session_files PUBLIC include)
target_link_libraries(sonora_session_files PUBLIC sonora_model PRIVATE sonora_platform sonora_sha256)

if(UNIX)
  add_executable(test_session_folder tests/test_session_folder.cpp)
  target_include_directories(test_session_folder PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_folder PRIVATE sonora_session_files)
  add_test(NAME session_folder COMMAND test_session_folder)
endif()
