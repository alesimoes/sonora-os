#!/usr/bin/env bash
# The build and test gate, run on this machine instead of GitHub's runners
# (owner, 2026-11-05: a 4-vCPU runner cannot build and test this tree in a
# useful time; the orchestrator runs this before it opens a PR and before
# the merge train merges one).
#
#   tools/local-ci.sh                  the contexts the change since main touches
#   tools/local-ci.sh --full           the whole Sonora software (main after a merge)
#   tools/local-ci.sh --os             the os-image context alone
#   tools/local-ci.sh --release        --full and --os (an image for the owner), and
#                                      keeps the plugin packages (Linux tar.gz,
#                                      Windows zip) in dist/
#   tools/local-ci.sh --base REF       compare with REF (default origin/main)
#   tools/local-ci.sh --dry-run        print contexts, steps and ctest filter, run none
#   tools/local-ci.sh --list           print the steps this run would take, one per line
#   tools/local-ci.sh --only A[,B...]  run just those steps of the computed set (the
#                                      context still decides the set; a name outside it
#                                      is an error). Combine with --full, --base, ...
#   tools/local-ci.sh --resume         skip the steps already green for this same HEAD
#
# Per-step results (2026-11-06: one 1 h serial pass that restarts from zero
# when a step fails or the machine dies): after each step the script writes
# $SONORA_LOCAL_CI_DIR/<step>.status, one line `state=pass|fail|running
# seconds=N sha=HEAD`. --resume trusts a `pass` only when its sha is the
# current HEAD, so a rerun after a crash or a fix pays for what is not green.
# The orchestrator (tools/harness/localci.py) posts each as the commit status
# `local-ci/<step>`; the aggregate `local-ci` stays the one required check.
#
# The owner's rule (2026-11-05): tests are separated by CONTEXT, like separate
# pipelines, and a PR pays only for its own context, so a small PR integrates
# in minutes; the whole suite runs once on main after the merge. A PR touching
# several contexts runs the union. The table in "which context" below is the
# one place that maps a path to a context.
#
#   context        paths                         steps
#   sonora-ui      ui/, keymaps/                 ui (or tree when another context
#                                                builds it) + the ui tests
#   sonora-plugins plugins/native|client|sdk|    tree (dsp/native tests), plugins,
#                  host, common/dsp              rtsan, windows. No TSan on a PR.
#   sonora-engine  engine/, drivers/, common/    tree, engine-scripts, tsan, rtsan,
#                  (not dsp), contracts/         record, bwf (engine/core,
#                                                common/audiofile), windows (common/)
#   os-image       iso/, audio-stack/            os (tools/build-iso.sh, then the
#                                                QEMU tests), quantum (audio-stack/)
#   harness/docs   tools/harness, .claude,       nothing heavy
#                  planning, brain, docs, *.md, .github
# A build script, Dockerfile or this script runs the three Sonora contexts.
#
# --full is the whole Sonora software: all three Sonora contexts, every test
# (slow ones included), ASan, and TSan including the plugins. The os-image
# context is not in --full (an image takes the machine for an hour or more);
# --os runs it alone and --release adds it to --full.
#
# In a PR run the tree builds incrementally (ccache, see tools/build-ui.sh) but
# ctest runs only the tests of the touched areas, by the module labels
# CMakeLists.txt gives every test (a ctest -L regex, SONORA_CTEST_LABELS), and
# never the tests labelled `slow` (the 12 long DSP suites, about 20 of the 26
# minutes the tree step spent testing; SONORA_SKIP_SLOW=1). A path with no
# label of its own, or one everything depends on (contracts/, common/ but
# dsp), runs the whole non-slow suite.
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
OS_ONLY=0
WITH_OS=0
STEP=""
ONLY=""
LIST=0
RESUME=0
while (($#)); do
  case "$1" in
    --full) FULL=1 ;;
    --os) OS_ONLY=1 ;;
    --release) FULL=1 WITH_OS=1; export SONORA_LOCAL_CI_RELEASE=1 ;;
    --base) BASE="$2"; shift ;;
    --dry-run) DRY=1 ;;
    --list) LIST=1 ;;
    --only) ONLY="${2:?--only needs STEP[,STEP...]}"; shift ;;
    --resume) RESUME=1 ;;
    --step) STEP="$2"; FULL=1; shift ;;   # internal: one step, lock held
    *) echo "usage: $0 [--full|--os|--release] [--base REF] [--dry-run|--list] [--only STEPS] [--resume]" >&2; exit 2 ;;
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

# --- which context the change touches ------------------------------------------
# Context flags, then per-path extras: bwf and portable (the Windows build)
# and ctest labels (module labels, CMakeLists.txt), or fullsuite when a path
# has no label of its own.
c_pluginhost=0 c_ui=0 c_plugins=0 c_engine=0 c_os=0 c_quantum=0 bwf=0 portable=0
LABELS=" " fullsuite=0
lab() { local l; for l; do [[ "$LABELS" == *" $l "* ]] || LABELS+="$l "; done; }
sonora() { c_pluginhost=1 c_ui=1 c_plugins=1 c_engine=1 portable=1 bwf=1 fullsuite=1; }
if ((OS_ONLY)); then
  c_os=1 FULL=0
elif ((FULL)); then
  sonora; c_quantum=1
  ((WITH_OS)) && c_os=1
else
  mb="$(git merge-base "$BASE" HEAD)" || { echo "error: no merge base with $BASE" >&2; exit 2; }
  files="$(git diff --no-renames --name-only "$mb" HEAD)"
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    # path -> context. First match wins.
    case "$f" in
      # harness/docs: nothing heavy
      planning/*|brain/*|docs/*|*.md|.github/*|.claude/*|tools/harness/*|tools/harness*.py|tools/orchestrator.py) ;;
      tools/dashboard*|tools/*.lua) ;;
      # os-image
      iso/*|tools/build-iso.sh|tools/test-*-qemu.sh|tools/make-stick-image.sh) c_os=1 ;;
      audio-stack/*) c_os=1 c_quantum=1 ;;
      # sonora-ui
      ui/*|keymaps/*) c_ui=1; lab ui ;;
      # sonora-plugins
      common/dsp/*) c_plugins=1 portable=1; lab dsp native ;;
      plugins/native/*) c_plugins=1 portable=1; lab native ;;
      plugins/client/*|plugins/sdk/*) c_plugins=1 c_pluginhost=1 portable=1; lab dsp native plugin_client ;;
      plugins/host/*) c_plugins=1 c_pluginhost=1; lab pluginhost plugin_client ;;
      plugins/*) c_plugins=1 c_pluginhost=1 fullsuite=1 ;;
      # sonora-engine
      engine/core/*) c_engine=1 bwf=1; lab core control app plugin_client drivers ui ;;
      common/audiofile/*) c_engine=1 bwf=1; lab audiofile core ;;
      engine/control/*) c_engine=1; lab control app ui ;;
      engine/app/*) c_engine=1; lab app ui ;;
      engine/plugin_client/*) c_engine=1; lab plugin_client core app ;;
      drivers/*) c_engine=1; lab drivers core app ;;
      engine/*|contracts/*) c_engine=1 fullsuite=1 ;;
      common/*) c_engine=1 portable=1 fullsuite=1 ;;
      # the Windows build alone
      tools/*.ps1) portable=1 ;;
      # build scripts, images and this script touch every Sonora context
      tools/build-*|tools/test-engine-docker.sh|tools/test-bwf-interop*.sh) sonora ;;
      tools/test-quantum-pin.sh|tools/rtsan-*|tools/failed-take-quit.py|tools/crash-recovery-kill.py) sonora ;;
      tools/*-entrypoint.sh|tools/verify-recording.py|tools/local-ci.sh) sonora ;;
      tools/Dockerfile*|tools/*.cmake|tools/*.supp) sonora ;;
      tools/*) ;;
      *) sonora ;;
    esac
  done <<<"$files"
fi

# --- the steps -----------------------------------------------------------------
steps=()
if ((c_engine || c_plugins)); then
  # The whole tree with Qt: every ctest test, engine and plugins included.
  steps+=(tree)
elif ((c_ui)); then
  # A UI-only change builds the UI and its tests only.
  steps+=(ui)
fi
((c_engine)) && steps+=(engine-scripts)
# plugins: the package's own build, tests and archive (common/* and
# plugins/native only); pluginhost: the whole tree with lilv, for the host.
((c_plugins)) && steps+=(plugins)
((c_pluginhost)) && steps+=(pluginhost)
# TSan: the engine's threads. The plugins' TSan runs on main (--full) only.
((c_engine || FULL)) && steps+=(tsan)
((FULL)) && steps+=(asan)
((c_engine || c_plugins)) && steps+=(rtsan)
((c_engine)) && steps+=(record)
((c_quantum)) && steps+=(quantum)
((c_engine && bwf)) && steps+=(bwf)
((c_os)) && steps+=(os)
# Windows last: the target is Linux, and a red Linux step skips it.
((c_plugins || portable)) && steps+=(windows)

if [[ -n "$ONLY" ]]; then
  # shellcheck disable=SC2206
  want=(${ONLY//,/ })
  picked=()
  for w in "${want[@]}"; do
    [[ " ${steps[*]} " == *" $w "* ]] || {
      echo "error: step '$w' is not in this run (${steps[*]:-none})" >&2; exit 2; }
  done
  for s in "${steps[@]}"; do
    [[ " ${want[*]} " == *" $s "* ]] && picked+=("$s")
  done
  steps=("${picked[@]}")
fi
if ((LIST)); then
  ((${#steps[@]})) && printf '%s\n' "${steps[@]}"
  exit 0
fi

contexts=()
((c_ui)) && contexts+=(sonora-ui)
((c_plugins)) && contexts+=(sonora-plugins)
((c_engine)) && contexts+=(sonora-engine)
((c_os)) && contexts+=(os-image)
if ((${#steps[@]} == 0)); then
  echo "local-ci: context: harness/docs"
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
  echo "local-ci: context: ${contexts[*]:-windows-build}"
  echo "local-ci: ${steps[*]}"
  [[ " ${steps[*]} " == *" tree "* || " ${steps[*]} " == *" ui "* ]] && echo "local-ci: ctest filter: $filter"
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
    plugins)
      # The archive is made after the tests pass; --release keeps it in dist/.
      pkg_out="$OUT/pkg"
      [[ -z "${SONORA_LOCAL_CI_RELEASE:-}" ]] || pkg_out="$ROOT/dist"
      tools/build-plugins.sh --plugins-only && tools/package-plugins.sh --out "$pkg_out" ;;
    pluginhost) tools/build-plugins.sh ;;
    tsan) sanitize thread ;;
    asan) sanitize address ;;
    rtsan) tools/build-rtsan.sh ;;
    record) tools/test-engine-docker.sh ;;
    quantum) tools/test-quantum-pin.sh ;;
    bwf) tools/test-bwf-interop.sh ;;
    os)
      # The image, then the QEMU tests against it: all of them run, the step
      # fails if any did. Each script takes the lock itself unless held.
      rc=0
      tools/build-iso.sh || return 1
      for t in test-boot-qemu test-install-qemu test-policycheck-qemu test-virtualcard-qemu; do
        timeout 3600 bash "tools/$t.sh" || { echo "FAIL $t"; rc=1; }
      done
      return $rc ;;
    windows)
      pkg_arg=()
      [[ -z "${SONORA_LOCAL_CI_RELEASE:-}" ]] || pkg_arg=(-Package -OutDir "$(host_path "$ROOT")/dist")
      powershell -NoProfile -ExecutionPolicy Bypass -File "$(host_path "$ROOT")/tools/build-portable.ps1" ${pkg_arg[@]+"${pkg_arg[@]}"} ;;
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

# <step>.status: see the header. Written through a temp file so the daemon
# never reads half a line.
HEAD_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
write_status() {  # step state seconds
  printf 'state=%s seconds=%s sha=%s\n' "$2" "$3" "$HEAD_SHA" >"$OUT/$1.status.tmp" &&
    mv -f "$OUT/$1.status.tmp" "$OUT/$1.status"
}
step_green() {  # step: passed before for this very HEAD
  [[ -f "$OUT/$1.status" ]] && grep -q "^state=pass .*sha=$HEAD_SHA\$" "$OUT/$1.status"
}

failed=()
for s in "${steps[@]}"; do
  if [[ "$s" == windows ]] && ((${#failed[@]})); then
    echo "skip windows (a Linux step failed)"
    continue
  fi
  if ((RESUME)) && step_green "$s"; then
    echo "ok   $s (resumed, green for $HEAD_SHA)"
    continue
  fi
  start=$SECONDS
  write_status "$s" running 0
  # A hung test or image build must not hold the machine's lock for good
  # (2026-11-05: playback_fake hung 68 min under TSan, a runtime image build
  # 70 min on apt): each step gets SONORA_STEP_TIMEOUT seconds, 3 h by default.
  if timeout "${SONORA_STEP_TIMEOUT:-10800}" "$SELF_DIR/build-slot.sh" bash "$SELF_DIR/local-ci.sh" --step "$s" >"$OUT/$s.log" 2>&1; then
    write_status "$s" pass $((SECONDS - start))
    echo "ok   $s ($((SECONDS - start)) s)"
  else
    write_status "$s" fail $((SECONDS - start))
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
