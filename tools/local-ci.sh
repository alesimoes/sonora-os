#!/usr/bin/env bash
# The build and test gate, run on this machine instead of GitHub's runners
# (owner, 2026-11-05: a 4-vCPU runner cannot build and test this tree in a
# useful time; the orchestrator runs this before it opens a PR and before
# the merge train merges one).
#
#   tools/local-ci.sh                  the steps the change since main needs
#   tools/local-ci.sh --full           every step (main after a merge)
#   tools/local-ci.sh --base REF       compare with REF (default origin/main)
#   tools/local-ci.sh --dry-run        print the steps, run none
#
# Two tiers (owner, 2026-11-05: integrate a small PR in minutes, run the whole
# suite once on main after the merge):
#   PR run (no --full)  builds the tree incrementally (ccache, see
#                       tools/build-ui.sh) but runs only the ctest tests of
#                       the areas the change touches, chosen by the module
#                       labels CMakeLists.txt gives every test (a ctest -L
#                       regex, SONORA_CTEST_LABELS), and never the tests
#                       labelled `slow` (the 12 long DSP suites, about 20 of
#                       the 26 minutes the tree step spent testing;
#                       SONORA_SKIP_SLOW=1). A path that maps to no label, or
#                       one everything depends on (contracts/, common/ but
#                       dsp), runs the whole non-slow suite. TSan, RTSan,
#                       record, BWF and Windows run only when the change
#                       touches their area; TSan follows the real-time flag.
#   --full              every step, every test, slow ones included.
# --dry-run prints the steps and the label filter for the change.
#
# Each step's output goes to $SONORA_LOCAL_CI_DIR/<step>.log (default
# .local-ci/ in the checkout); the summary on stdout is one line per step.
# Exit 0 only when every step passed.
set -uo pipefail

# The orchestrator runs its own copy of this script on another checkout
# (tools/harness/localci.py): SONORA_LOCAL_CI_ROOT names that checkout.
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${SONORA_LOCAL_CI_ROOT:-$(dirname "$SELF_DIR")}"
cd "$ROOT" || exit 2
BASE=origin/main
FULL=0
DRY=0
STEP=""
while (($#)); do
  case "$1" in
    --full) FULL=1 ;;
    --base) BASE="$2"; shift ;;
    --dry-run) DRY=1 ;;
    --step) STEP="$2"; FULL=1; shift ;;   # internal: one step, lock held
    *) echo "usage: $0 [--full] [--base REF] [--dry-run]" >&2; exit 2 ;;
  esac
  shift
done
OUT="${SONORA_LOCAL_CI_DIR:-$ROOT/.local-ci}"
# Docker on Windows wants a Windows-shaped host path (tools/build-engine.sh).
host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}
export MSYS2_ARG_CONV_EXCL='*'
mkdir -p "$OUT"

# --- which areas the change touches (the classifier .github/workflows/ci.yml
# used for its heavy jobs) ----------------------------------------------------
core=0 ui=0 plugins=0 audio=0 rt=0 bwf=0 rec=0 sanitizers=0 portable=0
# ctest labels of the changed areas (module labels, CMakeLists.txt), or the
# whole non-slow suite when a path has no label of its own.
LABELS=" " fullsuite=0
lab() { local l; for l; do [[ "$LABELS" == *" $l "* ]] || LABELS+="$l "; done; }
all() { core=1 ui=1 plugins=1 audio=1 rt=1 bwf=1 rec=1 portable=1 fullsuite=1; }
if ((FULL)); then
  all; sanitizers=1
else
  mb="$(git merge-base "$BASE" HEAD)" || { echo "error: no merge base with $BASE" >&2; exit 2; }
  files="$(git diff --no-renames --name-only "$mb" HEAD)"
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    case "$f" in
      planning/*|brain/*|docs/*|*.md) ;;
      engine/core/*) core=1 rt=1 bwf=1 rec=1; lab core control app plugin_client drivers ui ;;
      common/audiofile/*) core=1 rt=1 bwf=1 rec=1; lab audiofile core ;;
      engine/control/*) core=1 rt=1 rec=1; lab control app ui ;;
      engine/app/*) core=1 rt=1 rec=1; lab app ui ;;
      engine/plugin_client/*) core=1 rt=1 rec=1; lab plugin_client core app ;;
      drivers/*) core=1 rt=1 rec=1; lab drivers core app ;;
      engine/*) core=1 rt=1 rec=1 fullsuite=1 ;;
      contracts/*) core=1 rec=1 fullsuite=1 ;;
      common/dsp/*) core=1 rt=1 portable=1; lab dsp native ;;
      common/*) core=1 rt=1 rec=1 portable=1 fullsuite=1 ;;
      ui/*|keymaps/*) ui=1; lab ui ;;
      plugins/native/*) plugins=1 rt=1 portable=1; lab native ;;
      plugins/host/*) plugins=1; lab pluginhost plugin_client ;;
      plugins/client/*|plugins/sdk/*) plugins=1 rt=1 portable=1 fullsuite=1 ;;
      tools/*.ps1) portable=1 ;;
      plugins/*) plugins=1 fullsuite=1 ;;
      audio-stack/*) audio=1 ;;
      iso/*|.claude/*|tools/harness/*|tools/harness*.py|tools/orchestrator.py) ;;
      tools/dashboard*|tools/*.lua) ;;
      tools/build-*|tools/test-engine-docker.sh|tools/test-bwf-interop*.sh) all ;;
      tools/test-quantum-pin.sh|tools/rtsan-*|tools/failed-take-quit.py|tools/crash-recovery-kill.py) all ;;
      tools/*-entrypoint.sh|tools/verify-recording.py|tools/local-ci.sh) all ;;
      tools/Dockerfile*|tools/*.cmake|tools/*.supp) all ;;
      tools/*|.github/*) ;;
      *) all ;;
    esac
  done <<<"$files"
  # TSan where the audio thread or the threads around it are touched (rt:
  # engine/, drivers/, common/, plugins/native), not on every core change.
  ((rt)) && sanitizers=1
fi

# --- the steps -----------------------------------------------------------------
steps=()
if ((core || ui)); then
  # The whole tree with Qt: every ctest test, engine and plugins included.
  # A UI-only change builds the UI and its tests only.
  if ((core)); then steps+=(tree); else steps+=(ui); fi
fi
((core)) && steps+=(engine-scripts)
((plugins)) && steps+=(plugins)
((sanitizers)) && steps+=(tsan)
((FULL)) && steps+=(asan)
((rt)) && steps+=(rtsan)
((rec || audio)) && steps+=(record)
((audio)) && steps+=(quantum)
((bwf)) && steps+=(bwf)
# Windows last: the target is Linux, and a red Linux step skips it.
((portable)) && steps+=(windows)

if ((${#steps[@]} == 0)); then
  echo "local-ci: nothing to build or test (documentation or tooling only)"
  exit 0
fi
# What the tree step's ctest runs; the child steps inherit it from here.
filter="all tests"
if ((FULL)); then
  filter="all tests, slow ones included"
elif ((fullsuite)) || [[ "$LABELS" == " " ]]; then
  filter="all tests except label slow"
  export SONORA_SKIP_SLOW=1
else
  read -r -a lab_list <<<"$LABELS"
  SONORA_CTEST_LABELS="^($(IFS='|'; echo "${lab_list[*]}"))\$"
  export SONORA_CTEST_LABELS SONORA_SKIP_SLOW=1
  filter="-L '$SONORA_CTEST_LABELS' -LE slow"
fi
if [[ -z "$STEP" ]]; then
  echo "local-ci: ${steps[*]}"
  echo "local-ci: ctest filter: $filter"
fi
((DRY)) && exit 0

# One heavy job at a time on the machine, each with every CPU: each step
# takes the machine-wide lock (tools/build-slot.sh) for itself, so another
# run's steps can go between ours, and the scripts a step calls see
# SONORA_HEAVY_HELD and do not take it again.
# PRs run the slow DSP tests reduced (one rate, one setting); main runs them
# in full, in Release and under ASan (what main-full.yml ran on GitHub).
((FULL)) || export SONORA_DSP_QUICK=1

# A sanitizer tree in the build image (tools/Dockerfile.build), on its own
# volume so it never touches the Release tree.
sanitize() {  # thread | address
  local kind="$1" flags filter
  if [[ "$kind" == thread ]]; then
    # quoted: the filter is pasted into the container's bash -c script
    flags="-fsanitize=thread -g"; filter="-E '^(dsp|native)_'"
  else
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"; filter=""
  fi
  local id
  id="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  docker build -q -t sonora-build:trixie --label org.sonora=1 \
    -f "$(host_path "$ROOT/tools")/Dockerfile.build" "$(host_path "$ROOT/tools")" >/dev/null || return 1
  # TSan aborts with "unexpected memory mapping" under the 32-bit mmap
  # randomisation of Docker Desktop's kernel (GitHub's runners use 28). The
  # setting belongs to Docker's VM, not to this container, so a privileged
  # one lowers it; it resets when Docker restarts.
  if [[ "$kind" == thread ]]; then
    docker run --rm --privileged --entrypoint sysctl sonora-build:trixie \
      -qw vm.mmap_rnd_bits=28 || return 1
  fi
  docker run --rm --ulimit memlock=-1:-1 \
    ${SONORA_DSP_QUICK:+-e SONORA_DSP_QUICK=1} -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=10G \
    -v "sonora-$kind-$id:/build" -v sonora-ccache:/root/.ccache \
    -v "$(host_path "$ROOT"):/src" -w /src \
    sonora-build:trixie bash -ec "
      cmake -S . -B /build/t -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_FLAGS='$flags'
      cmake --build /build/t -j\$(nproc)
      ctest --test-dir /build/t --output-on-failure --timeout 900 -j\$(nproc) $filter
      $([[ "$kind" == thread ]] && echo "ctest --test-dir /build/t --output-on-failure --timeout 900 -j\$(nproc) -R ^native_format")"
}

run_step() {
  case "$1" in
    tree) tools/build-ui.sh ;;
    ui) SONORA_UI_SCOPE=ui tools/build-ui.sh ;;
    engine-scripts)
      # The two engine-process tests the engine job ran after ctest, against
      # the engine the tree step just built.
      docker run --rm --ulimit memlock=-1:-1 \
        -v "$(host_path "$ROOT"):/src" -w /src sonora-ui:trixie bash -ec '
          python3 tools/failed-take-quit.py --engine build-ui-container/sonora-engine
          python3 tools/crash-recovery-kill.py --engine build-ui-container/sonora-engine' ;;
    plugins) tools/build-plugins.sh ;;
    tsan) sanitize thread ;;
    asan) sanitize address ;;
    rtsan) tools/build-rtsan.sh ;;
    record) tools/test-engine-docker.sh ;;
    quantum) tools/test-quantum-pin.sh ;;
    bwf) tools/test-bwf-interop.sh ;;
    windows) powershell -NoProfile -ExecutionPolicy Bypass -File "$(host_path "$ROOT")/tools/build-portable.ps1" ;;
  esac
}

if [[ -n "$STEP" ]]; then
  run_step "$STEP"
  exit $?
fi

# A build killed while linking leaves an empty binary newer than its
# objects; make then never relinks it and the tests fail on "file too short".
find "$ROOT"/build-* -type f -size 0 \( -name '*.so' -o -name '*.a' -o -perm -u+x \)   -delete 2>/dev/null || true

# Off Linux, tools/build-ui.sh keeps the tree on a named volume that a fresh
# checkout creates root-owned; the image's user (uid 1000) cannot write it.
# tools/Dockerfile.ui now makes /build itself, but a branch from before that
# does not, so the volume is handed over here.
if [[ "$(uname -s)" != Linux ]] && docker image inspect sonora-ui:trixie >/dev/null 2>&1; then
  vol="sonora-build-$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  docker run --rm --user 0 -v "$vol:/build" --entrypoint chown sonora-ui:trixie 1000:1000 /build \
    >/dev/null 2>&1 || true
fi

failed=()
for s in "${steps[@]}"; do
  if [[ "$s" == windows ]] && ((${#failed[@]})); then
    echo "skip windows (a Linux step failed)"
    continue
  fi
  start=$SECONDS
  # A hung test or image build must not hold the machine's lock for good
  # (2026-11-05: playback_fake hung 68 min under TSan, a runtime image build
  # 70 min on apt): each step gets SONORA_STEP_TIMEOUT seconds, 3 h by default.
  if timeout "${SONORA_STEP_TIMEOUT:-10800}" "$SELF_DIR/build-slot.sh" bash "$SELF_DIR/local-ci.sh" --step "$s" >"$OUT/$s.log" 2>&1; then
    echo "ok   $s ($((SECONDS - start)) s)"
  else
    echo "FAIL $s ($((SECONDS - start)) s): $OUT/$s.log"
    failed+=("$s")
    # A batch of PRs only needs to know it is red: the orchestrator splits
    # it and reruns the halves, so the steps after the first failure would
    # be an hour spent on a verdict already known.
    if [[ "${SONORA_LOCAL_CI_FAIL_FAST:-0}" == 1 ]]; then
      echo "local-ci: stopped after the first failure (batch)"
      break
    fi
  fi
done
if ((${#failed[@]})); then
  echo "local-ci: failed: ${failed[*]}"
  exit 1
fi
echo "local-ci: all passed"
