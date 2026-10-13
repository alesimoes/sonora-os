# sonora-engine --backend fake records with no audio server: the whole
# engine, headless, on drivers/fake's own thread at real-time pace. Run with
# cmake -DENGINE=<path> -DDIR=<scratch dir> -P fake_engine_record.cmake.
#
# Passes on exit status 0 with all 18 UMC1820 channels connected and
# recorded. The engine refuses to start without unlimited memlock; where the
# limit cannot be raised (a developer's shell, a CI runner) that refusal is
# printed and ctest's SKIP_REGULAR_EXPRESSION turns it into a skip.
file(REMOVE_RECURSE "${DIR}")
execute_process(
  COMMAND "${ENGINE}" --backend fake --dir "${DIR}" --tracks auto --arm-all
          --record 1
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  TIMEOUT 30)
message("${out}")
message("${err}")
file(REMOVE_RECURSE "${DIR}")
if(err MATCHES "cannot lock memory")
  return()
endif()
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "sonora-engine --backend fake exited ${rc}")
endif()
if(NOT out MATCHES "RESULT files=18 connected=18 frames=48[0-9][0-9][0-9] requested=48000 rate=48000 buffer=128 xruns=0 clock_steps=0 overruns=0")
  message(FATAL_ERROR "no RESULT line with 18 files from 18 connected inputs")
endif()
