# Tube drive (ADR-0011 N2): the triode stage of common/dsp as a processor,
# held to ngspice on the same netlist (tests/spice/).
add_library(sonora_native_tube STATIC src/tube.cpp)
target_include_directories(sonora_native_tube PUBLIC include)
target_link_libraries(sonora_native_tube PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_tube_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_tube_suite PUBLIC sonora_native_tube sonora_dsp_measure)

sonora_native_quality_test(tube)
# The SPICE comparison at three oversampled rates and the aliasing tones at
# three rates and three qualities: one rate and the 1 kHz cases under
# SONORA_DSP_QUICK=1 (PR CI, sanitizers), everything in the nightly run.
set_tests_properties(native_tube PROPERTIES LABELS slow)

# The SPICE references re-derived from their netlists, where ngspice is
# installed (the sonora-plugins image): one ctest per case, so they run in
# parallel. tests/spice/ is regenerated with
#   sonora-dsp-measure --spice-generate plugins/native/tube/tests/spice --ngspice ngspice
find_program(SONORA_NGSPICE ngspice)
if(SONORA_NGSPICE)
  set(_spice_cases 16)  # measure/src/spice.cpp, triode_spice_cases()
  math(EXPR _last "${_spice_cases} - 1")
  foreach(i RANGE ${_last})
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/spice-work-${i})
    file(MAKE_DIRECTORY ${_work})
    add_test(NAME spice_triode_${i}
             COMMAND sonora-dsp-measure --spice-check ${CMAKE_CURRENT_SOURCE_DIR}/tests/spice
                     --ngspice ${SONORA_NGSPICE} --work ${_work} --case ${i} --cases ${_spice_cases})
    set_tests_properties(spice_triode_${i} PROPERTIES LABELS spice)
  endforeach()
else()
  message(STATUS "plugins/native/tube: ngspice not found; the spice_* reference checks are skipped")
endif()

# The LV2 and VST3 bundles, stereo and mono, through N1's format layer.
sonora_native_plugin(tube FACTORY make_tube DISPLAY "Sonora Tube Drive" UID SNtb MONO_UID SMtb
  LV2_CATEGORY "lv2:DistortionPlugin" VST3_CATEGORIES "Fx|Distortion")
