# Opto (ADR-0011 N2, slice 6): the optical cell of common/dsp as a
# levelling amplifier, held to the published circuit's behaviour and to
# ngspice on the same equations (tests/spice/).
add_library(sonora_native_opto STATIC src/opto.cpp)
target_include_directories(sonora_native_opto PUBLIC include)
target_link_libraries(sonora_native_opto PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_opto_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_opto_suite PUBLIC sonora_native_opto sonora_dsp_measure)

sonora_native_quality_test(opto)
# The static curves, the release protocol (holds up to 20 s) and the SPICE
# comparison at three rates: one rate and fewer points under
# SONORA_DSP_QUICK=1 (PR CI, sanitizers), everything in the nightly run.
set_tests_properties(native_opto PROPERTIES LABELS slow)

# The SPICE references re-derived from their netlists, where ngspice is
# installed (the sonora-plugins image), one ctest per case. tests/spice/ is
# regenerated with
#   sonora-dsp-measure --spice-kind opto --spice-generate plugins/native/opto/tests/spice --ngspice ngspice
find_program(SONORA_NGSPICE ngspice)
if(SONORA_NGSPICE)
  set(_spice_cases 6)  # measure/src/spice_opto.cpp, opto_spice_cases()
  math(EXPR _last "${_spice_cases} - 1")
  foreach(i RANGE ${_last})
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/spice-work-${i})
    file(MAKE_DIRECTORY ${_work})
    add_test(NAME spice_opto_${i}
             COMMAND sonora-dsp-measure --spice-kind opto --spice-check ${CMAKE_CURRENT_SOURCE_DIR}/tests/spice
                     --ngspice ${SONORA_NGSPICE} --work ${_work} --case ${i} --cases ${_spice_cases})
    set_tests_properties(spice_opto_${i} PROPERTIES LABELS spice)
  endforeach()
else()
  message(STATUS "plugins/native/opto: ngspice not found; the spice_opto_* reference checks are skipped")
endif()

# The LV2 and VST3 bundles, stereo and mono, with the key input.
sonora_native_plugin(opto FACTORY make_opto DISPLAY "Sonora Opto" UID SNop MONO_UID SMop
  LV2_CATEGORY "lv2:CompressorPlugin" VST3_CATEGORIES "Fx|Dynamics" SIDECHAIN)
