# Amp and cabinet (ADR-0011 stage N3). This slice: the `.nam` reader over
# common/dsp/neural's WaveNet kernel, its equivalence test against the NAM
# reference implementation's outputs (tests/oracle/), and the class-E cost
# measurement (tools/budget.cpp, ADR-0011 Unverified 9). The processor and
# its bundles come with the cabinet convolution.
add_library(sonora_native_ampcab_nam STATIC src/nam_file.cpp)
target_include_directories(sonora_native_ampcab_nam PUBLIC include)
target_link_libraries(sonora_native_ampcab_nam PUBLIC sonora_dsp PRIVATE sonora_json)

add_executable(test_native_ampcab tests/test_nam.cpp)
target_link_libraries(test_native_ampcab PRIVATE sonora_native_ampcab_nam)
add_test(NAME native_ampcab_nam
         COMMAND test_native_ampcab ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

# Not a test: per-block cost of the trainer's four WaveNet sizes at q128,
# printed for the record (`ampcab-budget [seconds]`).
add_executable(ampcab-budget tools/budget.cpp)
target_link_libraries(ampcab-budget PRIVATE sonora_dsp)
