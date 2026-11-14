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
#   SONORA_MICRO_BUILD_ONLY 1: scope all builds, runs no test (nightly's ccache seed)
set -euo pipefail

BUILD="${SONORA_BUILD_DIR:-build-micro}"
JOBS="${SONORA_JOBS:-$(nproc)}"
FILES="${SONORA_MICRO_FILES:?set SONORA_MICRO_FILES}"
PLAN="${SONORA_MICRO_PLAN:-$BUILD/micro-plan.json}"

if command -v ccache >/dev/null; then
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache CMAKE_C_COMPILER_LAUNCHER=ccache
  ccache -z >/dev/null 2>&1 || true
  # Also on a red run: the hit rate says whether the cache restore worked.
  trap 'ccache -s 2>&1 || true' EXIT
fi

# Ninja and mold when the image has them (Dockerfile.build/.ui): the link
# step of a test binary is most of what an incremental micro build costs.
# An existing tree keeps its generator (cmake refuses to change it).
gen=() link=()
if [[ ! -f "$BUILD/CMakeCache.txt" ]] && command -v ninja >/dev/null; then gen=(-G Ninja); fi
if command -v mold >/dev/null; then
  link=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold
        -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=mold)
fi
JUNIT="$(dirname "$PLAN")/ctest.xml"

# Each phase's wall time (tools/micro-build.sh prints the ones outside).
phase_t=$SECONDS
phase() { echo "micro-build: phase $1 $((SECONDS - phase_t)) s"; phase_t=$SECONDS; }
[[ -f "$BUILD/CMakeCache.txt" ]] && echo "micro-build: build tree $BUILD exists (incremental)"   || echo "micro-build: build tree $BUILD is new (cold)"
python3 tools/micro_targets.py query "$BUILD"
echo "micro-build: configuring ($BUILD, ${gen[*]:-existing generator}, linker $([[ ${#link[@]} -gt 0 ]] && echo mold || echo default))"
configure() {
  cmake -S . -B "$BUILD" "${gen[@]}" "${link[@]}" -DCMAKE_BUILD_TYPE=Release >"$BUILD.configure.log" 2>&1
}
# A failed try_compile (the compiler launcher could not write its cache, a
# missing header for one run) is stored in CMakeCache.txt and fails every
# later configure the same way, e.g. "Could NOT find Threads". A tree that
# existed before this run is reconfigured from scratch once; a second
# failure is real and is printed.
had_cache=0; [[ ! -f "$BUILD/CMakeCache.txt" ]] || had_cache=1
if ! configure; then
  if ((had_cache)); then
    echo "micro-build: configure failed on an existing tree; retrying from a clean cache"
    cat "$BUILD.configure.log"
    rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
    gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
    configure || { cat "$BUILD.configure.log"; exit 1; }
  else
    cat "$BUILD.configure.log"; exit 1
  fi
fi
grep -E "^-- SONORA_PLATFORM|^-- (ui/qt|plugins/host)" "$BUILD.configure.log" || true
phase configure

direct=()
[[ "${SONORA_MICRO_DIRECT:-0}" != 1 ]] || direct=(--direct)
mkdir -p "$(dirname "$PLAN")"
python3 tools/micro_targets.py plan "$BUILD" . --files-from "$FILES" "${direct[@]}" >"$PLAN"

scope=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["scope"])' "$PLAN")
mapfile -t targets < <(python3 -c 'import json,sys; [print(t) for t in json.load(open(sys.argv[1]))["targets"] if t]' "$PLAN")
mapfile -t tests < <(python3 -c '
import json, sys
p = json.load(open(sys.argv[1]))
t = p.get("tests")
print("\n".join(t) if isinstance(t, list) else "")' "$PLAN")

phase plan
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
    phase build
    if [[ "${SONORA_MICRO_BUILD_ONLY:-0}" == 1 ]]; then   # nightly's ccache seed
      echo "micro-build: build only (ccache seed)"; exit 0
    fi
    echo "micro-build: running every test (not slow)"
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure -j"$(nproc)" -LE slow       --output-junit "$JUNIT" ;;
  targets)
    if ((${#targets[@]})); then
      printf '  target %s\n' "${targets[@]}"
      cmake --build "$BUILD" -j"$JOBS" --target "${targets[@]}"
      phase build
    else
      # Every owner was an executable no test runs (pruned as a product).
      echo "micro-build: no library or test target to build"
    fi
    if ((${#tests[@]})); then
      regex=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ctest_regex"])' "$PLAN")
      echo "micro-build: ctest -R '$regex'"
      QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure \
        -j"$(nproc)" -LE slow -R "$regex"
      phase test
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
echo "micro-build: passed"
