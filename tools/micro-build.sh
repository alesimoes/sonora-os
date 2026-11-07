#!/usr/bin/env bash
# The micro build (ADR-0061): compile what the change touches, test what
# runs it, nothing else. The per-PR gate on GitHub's runners (ci.yml "Micro
# build"); the same run on a developer's machine, through the lock.
#
#   tools/micro-build.sh [--base REF] [--image build|ui] [--rtsan|--no-rtsan]
#                        [--dry-run] [--files-from F]
#
# The changed paths are the diff from the merge base with REF (default
# origin/main); --files-from reads them from a file instead. In the image
# (sonora-build, or sonora-ui when ui/ is touched, so Qt is there) the tree
# is configured once, tools/micro_targets.py maps the paths to targets and
# tests, and only those are built and run (tools/micro-build-entrypoint.sh).
# A change in the real-time call graph (engine/, drivers/, common/ but dsp,
# plugins/native, plugins/client) then runs the same targets' tests under
# RealtimeSanitizer (tools/build-rtsan.sh, affected targets only); --rtsan
# forces that stage, --no-rtsan skips it. Each stage takes the machine lock
# (tools/build-slot.sh) for itself. The plan is kept in .micro-build/.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
BASE=origin/main IMAGE="" DRY=0 RTSAN=auto FILES_FROM="" PLAN_ONLY=0
while (($#)); do
  case "$1" in
    --base) BASE="$2"; shift ;;
    --image) IMAGE="$2"; shift ;;
    --rtsan) RTSAN=1 ;;
    --no-rtsan) RTSAN=0 ;;
    # ci.yml's RTSan job: stage 1 only writes the plan (configure, no
    # build), stage 2 runs; the "Micro build" job did the Release build.
    --rtsan-only) RTSAN=1 PLAN_ONLY=1 ;;
    --dry-run) DRY=1 ;;
    --files-from) FILES_FROM="$2"; shift ;;
    *) echo "usage: $0 [--base REF] [--image build|ui] [--rtsan|--no-rtsan|--rtsan-only] [--dry-run] [--files-from F]" >&2; exit 2 ;;
  esac
  shift
done
OUT="$ROOT/.micro-build"
mkdir -p "$OUT"

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}
[[ "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* ]] || export MSYS2_ARG_CONV_EXCL='*'

# --- the changed paths ------------------------------------------------------------
if [[ -n "$FILES_FROM" ]]; then
  grep -v '^$' "$FILES_FROM" >"$OUT/files" || true
else
  mb="$(git merge-base "$BASE" HEAD)" || { echo "error: no merge base with $BASE" >&2; exit 2; }
  git diff --no-renames --name-only "$mb" HEAD >"$OUT/files"
fi
if [[ ! -s "$OUT/files" ]]; then
  echo "micro-build: no changed files; nothing to build"
  exit 0
fi

# Which image, and whether the real-time stage applies. The table of RT
# paths is ci.yml's `rt` output: engine, drivers, common
# but dsp, the native processors and the plugin client.
ui=0 rt=0 build=0
while IFS= read -r f; do
  case "$f" in
    ui/*|keymaps/*) ui=1 build=1 ;;
    engine/*|drivers/*|plugins/client/*|plugins/native/*|plugins/sdk/*) rt=1 build=1 ;;
    common/dsp/*) build=1 ;;
    common/*|contracts/*) rt=1 build=1 ;;
    plugins/*|interchange/*|CMakeLists.txt|cmake/*|tools/*.cmake) build=1 ;;
    *) ;;
  esac
done <"$OUT/files"
[[ -n "$IMAGE" ]] || IMAGE=$([[ $ui == 1 ]] && echo ui || echo build)
[[ "$RTSAN" != auto ]] || RTSAN=$rt

echo "micro-build: $(wc -l <"$OUT/files" | tr -d ' ') changed paths, image sonora-$IMAGE, rtsan=$RTSAN"
if ((!build)); then
  echo "micro-build: no path under a CMake module; nothing to build"
  exit 0
fi
((DRY)) && { cat "$OUT/files"; exit 0; }

# --- stage 1: build and test the affected targets -------------------------------------
command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker image inspect "sonora-$IMAGE:trixie" >/dev/null 2>&1 || {
  # On GitHub the image comes from GHCR (the sonora-image action, which
  # builds only when the change edits the image's inputs); building it here
  # would put minutes of apt into every PR.
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "error: sonora-$IMAGE:trixie is not loaded; the job must pull it with ./.github/actions/sonora-image" >&2
    exit 1
  fi
  if [[ "$IMAGE" == ui ]]; then
    ctx="$(mktemp -d)"; mkdir -p "$ctx/audio-stack"
    cp tools/Dockerfile.ui "$ctx/Dockerfile"; cp audio-stack/pipewire-sonora.conf "$ctx/audio-stack/"
    docker build -q -t sonora-ui:trixie --label org.sonora=1 "$(host_path "$ctx")" >/dev/null || exit 1
    rm -rf "$ctx"
  else
    docker build -q -t sonora-build:trixie --label org.sonora=1 \
      -f "$(host_path "$ROOT/tools")/Dockerfile.build" "$(host_path "$ROOT/tools")" >/dev/null || exit 1
  fi
}
JOBS="$("$ROOT/tools/build-slot.sh" --jobs)"
ID="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
# Off Linux the tree lives on a volume per checkout (NTFS bind mounts are
# slow); one directory per image, as the Qt one differs. The ccache is the
# volume every worktree shares, or SONORA_CCACHE_DIR (a host directory the
# GitHub job restores from its cache).
VOL=() AS_CALLER=()
if [[ "$(uname -s)" != Linux ]]; then
  VOL=(-v "sonora-build-$ID:/build" -e "SONORA_BUILD_DIR=/build/micro-$IMAGE")
  docker run --rm --user 0 -v "sonora-build-$ID:/build" --entrypoint chown "sonora-$IMAGE:trixie" \
    -R 1000:1000 /build >/dev/null 2>&1 || true
else
  VOL=(-e "SONORA_BUILD_DIR=build-micro-$IMAGE")
  AS_CALLER=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
fi
if [[ -n "${SONORA_CCACHE_DIR:-}" ]]; then
  mkdir -p "$SONORA_CCACHE_DIR"
  CCACHE=(-v "$(host_path "$SONORA_CCACHE_DIR"):/ccache")
elif [[ "$(uname -s)" == Linux ]]; then
  # A host directory, not a named volume: a fresh volume is root's, and the
  # container runs as the caller (ccache: Permission denied broke the RTSan
  # job's configure on PRs #478 and #479, 2026-11-07).
  SONORA_CCACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/sonora-ccache"
  mkdir -p "$SONORA_CCACHE_DIR"
  CCACHE=(-v "$SONORA_CCACHE_DIR:/ccache")
else
  CCACHE=(-v sonora-ccache:/ccache)
fi
# The table in the job's summary page (changed files, targets, tests, times),
# also when a stage fails.
SUMMARY_TITLE="Build + test"
if [[ "$RTSAN" == 1 && "$PLAN_ONLY" == 1 ]]; then SUMMARY_TITLE="RTSan"; fi
summary() {
  [[ -z "${GITHUB_STEP_SUMMARY:-}" ]] ||
    python3 "$ROOT/tools/micro_summary.py" "$OUT" "$SUMMARY_TITLE" >>"$GITHUB_STEP_SUMMARY" || true
}
trap summary EXIT
echo "micro-build: image sonora-$IMAGE:trixie, phases: configure, build, test"
start=$SECONDS
if ! "$ROOT/tools/build-slot.sh" docker run --rm "${AS_CALLER[@]}" -e LANG=C.UTF-8 -e SONORA_JOBS="$JOBS" \
    -e SONORA_MICRO_FILES=/src/.micro-build/files -e SONORA_MICRO_PLAN=/src/.micro-build/plan.json \
    -e SONORA_MICRO_DIRECT="${SONORA_MICRO_DIRECT:-0}" -e SONORA_MICRO_BUILD_ONLY="${SONORA_MICRO_BUILD_ONLY:-0}" -e SONORA_MICRO_PLAN_ONLY="$PLAN_ONLY" -e SONORA_DSP_QUICK=1 \
    -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}" -e CCACHE_COMPRESS=1 \
    "${VOL[@]}" "${CCACHE[@]}" -v "$(host_path "$ROOT"):/src" -w /src \
    "sonora-$IMAGE:trixie" tools/micro-build-entrypoint.sh; then
  echo "micro-build: FAIL build ($((SECONDS - start)) s)"
  exit 1
fi
((PLAN_ONLY)) || echo "micro-build: ok build ($((SECONDS - start)) s)"

# --- stage 2: the same targets under RealtimeSanitizer ---------------------------------
if [[ "$RTSAN" == 1 && -f "$OUT/plan.json" ]]; then
  scope=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["scope"])' "$OUT/plan.json" 2>/dev/null || echo none)
  if [[ "$scope" == none ]]; then
    echo "micro-build: rtsan skipped (nothing built)"
  else
    targets=$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["targets"]))' "$OUT/plan.json")
    regex=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("ctest_regex") or "")' "$OUT/plan.json")
    args=()
    [[ "$scope" == all ]] || { export SONORA_RTSAN_TARGETS="$targets"; args=(-R "${regex:-^$}"); }
    echo "micro-build: rtsan: building and running the tests of: $targets"
    start=$SECONDS
    if ! SONORA_RTSAN_LIVE=0 "$ROOT/tools/build-rtsan.sh" "${args[@]}"; then
      echo "micro-build: FAIL rtsan ($((SECONDS - start)) s)"
      exit 1
    fi
    echo "micro-build: ok rtsan ($((SECONDS - start)) s)"
  fi
fi
echo "micro-build: all passed"
