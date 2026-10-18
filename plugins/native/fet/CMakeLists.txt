# FET (ADR-0011 N2, slice 6b): the FET stage of common/dsp as a peak
# limiter, held to the published circuit's figures and to ngspice on the
# same reduced circuit with ngspice's own JFET and diode models
# (tests/spice/; measure/spice_fet.h says what is behavioural).
add_library(sonora_native_fet STATIC src/fet.cpp)
target_include_directories(sonora_native_fet PUBLIC include)
target_link_libraries(sonora_native_fet PUBLIC sonora_native_sdk PRIVATE sonora_dsp)

add_library(sonora_native_fet_suite STATIC tests/suite.cpp)
target_link_libraries(sonora_native_fet_suite PUBLIC sonora_native_fet sonora_dsp_measure)

sonora_native_quality_test(fet)
# The static curves (five positions, settled over seconds for C9), the
# release protocol and the SPICE comparison at several rates: one rate and
# fewer points under SONORA_DSP_QUICK=1 (PR CI, sanitizers), everything in
# the nightly run.
set_tests_properties(native_fet PROPERTIES LABELS slow)

# The SPICE references re-derived from their netlists, where ngspice is
# installed (the sonora-plugins image), one ctest per case. tests/spice/ is
# regenerated with
#   sonora-dsp-measure --spice-kind fet --spice-generate plugins/native/fet/tests/spice --ngspice ngspice
find_program(SONORA_NGSPICE ngspice)
if(SONORA_NGSPICE)
  set(_spice_cases 6)  # measure/src/spice_fet.cpp, fet_spice_cases()
  math(EXPR _last "${_spice_cases} - 1")
  foreach(i RANGE ${_last})
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/spice-work-${i})
    file(MAKE_DIRECTORY ${_work})
    add_test(NAME spice_fet_${i}
             COMMAND sonora-dsp-measure --spice-kind fet --spice-check ${CMAKE_CURRENT_SOURCE_DIR}/tests/spice
                     --ngspice ${SONORA_NGSPICE} --work ${_work} --case ${i} --cases ${_spice_cases})
    set_tests_properties(spice_fet_${i} PROPERTIES LABELS spice)
  endforeach()
else()
  message(STATUS "plugins/native/fet: ngspice not found; the spice_fet_* reference checks are skipped")
endif()

# The LV2 and VST3 bundles, stereo and mono, with the key input.
sonora_native_plugin(fet FACTORY make_fet DISPLAY "Sonora FET" UID SNfe MONO_UID SMfe
  LV2_CATEGORY "lv2:LimiterPlugin" VST3_CATEGORIES "Fx|Dynamics" SIDECHAIN)
