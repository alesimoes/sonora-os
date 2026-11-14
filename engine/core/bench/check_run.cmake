# Runs a command and checks both its exit code and its output, which a
# ctest PASS_REGULAR_EXPRESSION alone cannot (it ignores the exit code).
#   -DBENCH=path -DARGS="a;b;c" -DEXPECT_EXIT=N -DEXPECT_REGEX=... [-DREJECT_REGEX=...]
# BENCH is an argument of its own so tools/micro_targets.py sees the
# executable in the test's command and builds it for the test.
execute_process(COMMAND ${BENCH} ${ARGS} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(NOT rc STREQUAL "${EXPECT_EXIT}")
  message(FATAL_ERROR "exit ${rc}, expected ${EXPECT_EXIT}")
endif()
if(NOT out MATCHES "${EXPECT_REGEX}")
  message(FATAL_ERROR "output does not match: ${EXPECT_REGEX}")
endif()
if(DEFINED REJECT_REGEX AND out MATCHES "${REJECT_REGEX}")
  message(FATAL_ERROR "output matches what it must not: ${REJECT_REGEX}")
endif()
string(REGEX MATCHALL "RESULT " results "${out}")
list(LENGTH results n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "${n} RESULT lines, expected exactly one")
endif()
