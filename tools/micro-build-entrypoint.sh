#!/usr/bin/env bash
# Runs inside a build image (sonora-build or sonora-ui); tools/micro-build.sh
# is the way in. Configures the tree once (seconds, cached), asks
# tools/micro_targets.py which targets the changed files reach, builds those
# targets only and runs only their tests (ADR-0061).
#
#   SONORA_MICRO_FILES   file listing the changed paths, one per line
#   SONORA_MICRO_PLAN    where the plan JSON is written (for the rtsan stage
#                        and the summary); default $SONORA_BUILD_DIR/micro-plan.json
#   SONORA_BUILD_DIR     build tree (default build-micro)
#   SONORA_JOBS          -j for the build (default nproc)
#   SONORA_MICRO_DIRECT  1: direct dependents only (default: transitive)
#   SONORA_MICRO_PLAN_ONLY  1: write the plan and stop (the RTSan job's input)
set -euo pipefail

BUILD="${SONORA_BUILD_DIR:-build-micro}"
JOBS="${SONORA_JOBS:-$(nproc)}"
FILES="${SONORA_MICRO_FILES:?set SONORA_MICRO_FILES}"
PLAN="${SONORA_MICRO_PLAN:-$BUILD/micro-plan.json}"

if command -v ccache >/dev/null; then
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache CMAKE_C_COMPILER_LAUNCHER=ccache
  # ui/qt precompiles the Qt headers (SONORA_UI_PCH); ccache caches those units
  # only with this sloppiness, else it falls back to the real compiler.
  export CCACHE_SLOPPINESS=pch_defines,time_macros
  ccache -z >/dev/null 2>&1 || true
fi

python3 tools/micro_targets.py query "$BUILD"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >"$BUILD.configure.log" 2>&1 || {
  cat "$BUILD.configure.log"; exit 1
}
grep -E "^-- SONORA_PLATFORM|^-- (ui/qt|plugins/host)" "$BUILD.configure.log" || true

direct=()
[[ "${SONORA_MICRO_DIRECT:-0}" != 1 ]] || direct=(--direct)
mkdir -p "$(dirname "$PLAN")"
python3 tools/micro_targets.py plan "$BUILD" . --files-from "$FILES" "${direct[@]}" >"$PLAN"

scope=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["scope"])' "$PLAN")
mapfile -t targets < <(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["targets"]))' "$PLAN")
mapfile -t tests < <(python3 -c '
import json, sys
p = json.load(open(sys.argv[1]))
t = p.get("tests")
print("\n".join(t) if isinstance(t, list) else "")' "$PLAN")

echo "micro-build: scope $scope, ${#targets[@]} targets, ${#tests[@]} tests"
python3 -c '
import json, sys
for k, v in sorted(json.load(open(sys.argv[1]))["why"].items()):
    print(f"  {k}: {v}")' "$PLAN"

[[ "${SONORA_MICRO_PLAN_ONLY:-0}" != 1 ]] || { echo "micro-build: plan written to $PLAN"; exit 0; }
case "$scope" in
  none)
    echo "micro-build: nothing to build (no target owns the changed files)"
    exit 0 ;;
  all)
    echo "micro-build: a root build file changed; building the whole tree"
    cmake --build "$BUILD" -j"$JOBS"
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure -j"$(nproc)" -LE slow ;;
  targets)
    printf '  target %s\n' "${targets[@]}"
    cmake --build "$BUILD" -j"$JOBS" --target "${targets[@]}"
    if ((${#tests[@]})); then
      regex=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ctest_regex"])' "$PLAN")
      echo "micro-build: ctest -R '$regex'"
      QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure \
        -j"$(nproc)" -LE slow -R "$regex"
    else
      echo "micro-build: no test runs an affected target"
    fi ;;
esac
# The engine-process integration tests (a quit during a failed take, a kill
# mid-take then reopen) are scripts, not ctest tests: they run whenever the
# engine was among the targets (the plan keeps it when a test runs it).
if [[ " ${targets[*]} " == *" sonora-engine "* && -x "$BUILD/sonora-engine" ]]; then
  echo "micro-build: engine-process tests"
  python3 tools/failed-take-quit.py --engine "$BUILD/sonora-engine"
  python3 tools/crash-recovery-kill.py --engine "$BUILD/sonora-engine"
fi
if command -v ccache >/dev/null; then
  ccache -s 2>/dev/null | grep -iE "hit|cache size" || true
fi
echo "micro-build: passed"
