# sonora-analyze (ADR-0011 3.4, ADR-0046): offline analysis passes and the mix
# assistant's planner. Never on an audio thread; it allocates and reads files.
# Linux-only for now because it links common/model (the plan contract,
# ADR-0038), which is.
#
# This file is the single owner of the module's build (ADR-0066 §5). A pass
# adds its sources and tests in its own fragment, cmake/<pass>.cmake, which is
# included below when it exists; it never edits this file.
add_library(sonora_analyze STATIC
  src/mastering_plan.cpp
  src/mastering_rules.cpp
  src/mix_analysis.cpp)
target_include_directories(sonora_analyze PUBLIC include)
target_link_libraries(sonora_analyze PUBLIC sonora_model sonora_native_mastering sonora_json PRIVATE sonora_dsp)

# ONNX Runtime (ADR-0066 §2): only this module links it, only when asked, and a
# build without it is a full build (the classical passes are the default).
# cmake/models.cmake adds the runner source when SONORA_ONNX_FOUND.
option(SONORA_WITH_ONNX "Link ONNX Runtime into sonora-analyze for the model-backed passes" OFF)
set(SONORA_ONNX_FOUND FALSE)
if(SONORA_WITH_ONNX)
  find_library(SONORA_ONNX_LIBRARY onnxruntime)
  find_path(SONORA_ONNX_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime onnxruntime/core/session)
  if(SONORA_ONNX_LIBRARY AND SONORA_ONNX_INCLUDE_DIR)
    set(SONORA_ONNX_FOUND TRUE)
  else()
    message(STATUS "sonora-analyze: ONNX Runtime not found; model-backed passes disabled")
  endif()
endif()

# The subcommands (app/commands.h): a pass's real command file replaces its
# stub by existing; fragments do not add app/cmd_*.cpp.
set(_sonora_analyze_app app/main.cpp)
foreach(_cmd key timbre tone vocal)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/cmd_${_cmd}.cpp")
    list(APPEND _sonora_analyze_app app/cmd_${_cmd}.cpp)
  else()
    list(APPEND _sonora_analyze_app app/cmd_${_cmd}_stub.cpp)
  endif()
endforeach()
add_executable(sonora-analyze ${_sonora_analyze_app})
target_link_libraries(sonora-analyze PRIVATE sonora_analyze sonora_audiofile sonora_json)

add_executable(test_analyze_mastering tests/test_mastering.cpp)
target_link_libraries(test_analyze_mastering PRIVATE sonora_analyze sonora_dsp)
add_test(NAME analyze_mastering COMMAND test_analyze_mastering)

add_executable(test_analyze_contract tests/test_analyze_contract.cpp)
target_link_libraries(test_analyze_contract PRIVATE sonora_analyze)
add_test(NAME analyze_contract COMMAND test_analyze_contract)

# The tone-match contract (ADR-0072): profile schema, staleness, fakes.
add_executable(test_analyze_tone_contract tests/test_tone_contract.cpp)
target_link_libraries(test_analyze_tone_contract PRIVATE sonora_analyze)
add_test(NAME analyze_tone_contract COMMAND test_analyze_tone_contract)
# The root labels no plugins/analyze test yet; this one carries its module's.
set_tests_properties(analyze_tone_contract PROPERTIES LABELS analyze)

# The command line: exit code and message together (ctest alone can check one
# or the other). `sh` is fine here: the module is Linux-only.
# No ';' in the script: CMake would read it as a list separator.
set(_sonora_analyze_expect [=[out=$("$0" "$@" 2>&1)
rc=$?
echo "rc=$rc $out"
[ "$rc" -eq "$EXPECT_RC" ] || exit 1
printf '%s' "$out" | grep -qF "$EXPECT_TEXT"]=])
function(sonora_analyze_cli_test name rc text)
  add_test(NAME ${name} COMMAND sh -c "${_sonora_analyze_expect}" $<TARGET_FILE:sonora-analyze> ${ARGN})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "EXPECT_RC=${rc};EXPECT_TEXT=${text}")
endfunction()
sonora_analyze_cli_test(analyze_cli_usage 2 "usage: sonora-analyze mastering" frobnicate)
sonora_analyze_cli_test(analyze_cli_mastering_usage 2 "usage: sonora-analyze mastering" mastering)
# Only while the stub is the one built; the pass's own tests replace these.
foreach(_cmd key timbre tone)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/cmd_${_cmd}.cpp")
    sonora_analyze_cli_test(analyze_cli_${_cmd}_stub 1 "${_cmd}: not implemented" ${_cmd})
  endif()
endforeach()

# Vocal Balance (ADR-0104): the subcommand is `vocal-balance`, the file is
# cmd_vocal. The usage line holds for the stub and for the real command.
sonora_analyze_cli_test(analyze_cli_vocal_usage 2 "usage: sonora-analyze vocal-balance" vocal-balance)

foreach(_fragment models key timbre tone)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_fragment}.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_fragment}.cmake")
  endif()
endforeach()
