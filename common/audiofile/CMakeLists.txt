add_library(sonora_audiofile STATIC src/wav_writer.cpp src/wav_reader.cpp src/wav_recovery.cpp
  src/wrap_sidecar.cpp src/audio_export.cpp src/export_file.cpp src/audio_import.cpp src/peak_file.cpp)
target_include_directories(sonora_audiofile PUBLIC include)
# common/platform for the take file's exclusive, close-on-exec create and its
# periodic sync (ADR-0008 Â§3; ADR-0009 Consequences).
target_link_libraries(sonora_audiofile PUBLIC sonora_base PRIVATE sonora_platform)

add_executable(test_wav_writer tests/test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE sonora_audiofile)
add_test(NAME wav_writer COMMAND test_wav_writer)

add_executable(test_wav_reader tests/test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE sonora_audiofile)
add_test(NAME wav_reader COMMAND test_wav_reader)

# A child process started while a take is open does not inherit it
# (ADR-0008 V3: OS-specific tests live in tests/posix, built if(UNIX)).
if(UNIX)
  # Needs the POSIX create_exclusive, not the unsupported stub's.
  if(SONORA_PLATFORM STREQUAL "linux")
    add_executable(test_wav_writer_cloexec tests/posix/test_wav_writer_cloexec.cpp)
    target_link_libraries(test_wav_writer_cloexec PRIVATE sonora_audiofile sonora_platform)
    add_test(NAME wav_writer_cloexec COMMAND test_wav_writer_cloexec)
  endif()

  # A file-size limit stands in for a full disk (RLIMIT_FSIZE).
  add_executable(test_wav_writer_full_disk tests/posix/test_wav_writer_full_disk.cpp)
  target_link_libraries(test_wav_writer_full_disk PRIVATE sonora_audiofile)
  add_test(NAME wav_writer_full_disk COMMAND test_wav_writer_full_disk)
endif()

# A take whose engine died before closing it gets the length on disk
# (session-format.md, "Crash recovery"); a finished or foreign file is left
# as it is.
add_executable(test_wav_recovery tests/test_wav_recovery.cpp)
target_link_libraries(test_wav_recovery PRIVATE sonora_audiofile)
add_test(NAME wav_recovery COMMAND test_wav_recovery)

# A loop take's wrap list next to its file (ADR-0016 §2).
add_executable(test_wrap_sidecar tests/test_wrap_sidecar.cpp)
target_link_libraries(test_wrap_sidecar PRIVATE sonora_audiofile)
add_test(NAME wrap_sidecar COMMAND test_wrap_sidecar)

# Delivery files (interchange.audio.export): BWF, WAV, AIFF and FLAC at 16,
# 24 and 32-bit float read back by parsers that share no code with the
# writers, bext fields, dither statistics, rate conversion. Portable.
add_executable(test_audio_export tests/test_audio_export.cpp)
target_link_libraries(test_audio_export PRIVATE sonora_audiofile)
add_test(NAME audio_export COMMAND test_audio_export)

# Import: any WAV to the session rate, as a BWF copy (ui.editing.import).
add_executable(test_audio_import tests/test_audio_import.cpp)
target_link_libraries(test_audio_import PRIVATE sonora_audiofile)
add_test(NAME audio_import COMMAND test_audio_import)

# The overview file beside a take: multi-resolution min/max, built off the
# audio thread (ui.editing.waveform).
add_executable(test_peak_file tests/test_peak_file.cpp)
target_link_libraries(test_peak_file PRIVATE sonora_audiofile)
add_test(NAME peak_file COMMAND test_peak_file)
