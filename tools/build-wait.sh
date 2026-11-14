#!/usr/bin/env bash
# Collects a detached build (tools/build-slot.sh --detach): blocks until it
# ends or for at most --max seconds (default 540, under an agent's ten-minute
# command limit), then prints its state. Exit: the build's own status once it
# ended; 75 while it is still queued or running (call again, nothing is
# lost); 70 when its runner died without a result.
#
#   tools/build-wait.sh [ID] [--max S] [--tail N]
exec "$(dirname "${BASH_SOURCE[0]}")/build-job.sh" wait "$@"
