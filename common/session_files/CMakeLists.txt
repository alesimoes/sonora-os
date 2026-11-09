# A session folder on disk (ADR-0015 §3, §6; ADR-0007 update of
# 2026-10-25). Layer 2: the model's bytes, the platform's syncs. The
# engine's saver thread links it; the interface's tests link it to stand in
# for the engine, and the interface itself does not. Never on the audio
# thread.
add_library(sonora_session_files STATIC src/session_folder.cpp src/session_import.cpp)
target_include_directories(sonora_session_files PUBLIC include)
target_link_libraries(sonora_session_files PUBLIC sonora_model PRIVATE sonora_platform sonora_sha256)

# Import Session Data (ADR-0123): a pure planner over two snapshots, no disk
# and no POSIX, so it runs on every platform the library builds on.
add_executable(test_session_import tests/test_session_import.cpp)
target_link_libraries(test_session_import PRIVATE sonora_session_files)
add_test(NAME session_import COMMAND test_session_import)

if(UNIX)
  add_executable(test_session_folder tests/test_session_folder.cpp)
  target_include_directories(test_session_folder PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_folder PRIVATE sonora_session_files sonora_sha256)
  add_test(NAME session_folder COMMAND test_session_folder)

  # Rotated backups, the autosave and Save As (daw.model.session_mgmt): a
  # fork killed mid-write, symlinks and hard links, so POSIX's.
  add_executable(test_session_copies tests/test_session_copies.cpp)
  target_include_directories(test_session_copies PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_copies PRIVATE sonora_session_files)
  add_test(NAME session_copies COMMAND test_session_copies)

  # Session templates (ADR-0030): Save as Template and New from Template as
  # the engine's Save Copy In and Save As run them.
  add_executable(test_session_templates tests/test_session_templates.cpp)
  target_include_directories(test_session_templates PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_templates PRIVATE sonora_session_files sonora_sha256)
  add_test(NAME session_templates COMMAND test_session_templates)

  # Plugin files in the session (ADR-0029 slice 2): placed, synced, carried
  # by Save As and the untitled rename, never through a symlink.
  add_executable(test_session_plugin_files tests/test_session_plugin_files.cpp)
  target_include_directories(test_session_plugin_files PRIVATE ${CMAKE_SOURCE_DIR}/common/model/tests)
  target_link_libraries(test_session_plugin_files PRIVATE sonora_session_files sonora_sha256)
  add_test(NAME session_folder_plugin_files COMMAND test_session_plugin_files)
endif()
