# Two sonora-engine --backend fake started together on one session folder:
# one records, the other is refused with a reason. Without the folder lock
# the second ran crash recovery over the first one's open takes. --record
# opens no control socket, so the socket cannot be what refuses it. Run with
# cmake -DENGINE=<path> -DDIR=<scratch dir> -P fake_engine_two_engines.cmake.
#
# Two COMMANDs make a pipeline, so both start at once; each records for 3 s,
# far longer than either takes to reach the lock. Each writes to its own
# log, so the one refused first closing the pipe cannot SIGPIPE the other.
file(REMOVE_RECURSE "${DIR}")
file(REMOVE "${DIR}.a.log" "${DIR}.b.log")
set(run "\"$0\" --backend fake --dir \"$1\" --tracks 2 --arm-all --record 3 >\"$2\" 2>&1")
execute_process(
  COMMAND sh -c "${run}" "${ENGINE}" "${DIR}" "${DIR}.a.log"
  COMMAND sh -c "${run}" "${ENGINE}" "${DIR}" "${DIR}.b.log"
  RESULTS_VARIABLE rcs
  TIMEOUT 30)
file(READ "${DIR}.a.log" a)
file(READ "${DIR}.b.log" b)
message("${a}")
message("${b}")
file(REMOVE_RECURSE "${DIR}")
file(REMOVE "${DIR}.a.log" "${DIR}.b.log")
if("${a}${b}" MATCHES "cannot lock memory")
  return()
endif()
list(SORT rcs)
if(NOT rcs STREQUAL "0;1")
  message(FATAL_ERROR "exit statuses ${rcs}: want one engine to record and one refused")
endif()
if(NOT "${a}${b}" MATCHES "another engine is using [^\n]*; one session takes one engine")
  message(FATAL_ERROR "the refused engine did not say why")
endif()
if(NOT "${a}${b}" MATCHES "RESULT files=2 connected=2 ")
  message(FATAL_ERROR "the engine that held the folder did not record")
endif()
