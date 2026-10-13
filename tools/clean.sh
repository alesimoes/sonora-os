#!/usr/bin/env bash
# Reclaim the disk SonoraOS builds and tests use up, and nothing else.
#
#   tools/clean.sh --dry-run    list what would go, with sizes
#   tools/clean.sh              remove it
#   tools/clean.sh --deep       also the live-build cache volume (an ISO
#                               build then takes about 20 min instead of 10)
#   tools/clean.sh --days N     age for build-*/ output dirs (default 3)
#
# Ours means: a Docker object labelled org.sonora=1 or named sonora-*, a git
# worktree under .claude/worktrees, a build-*/ directory in the main checkout.
# The host also runs the owner's other projects in Docker; anything not ours
# is never touched, however unused it looks.
#
# What goes:
#   - stopped containers of ours
#   - dangling images of ours (labelled, or built from a Dockerfile of ours)
#   - images of ours no script in tools/ uses (KEEP_IMAGES below)
#   - volumes of ours, except sonora-livebuild-work without --deep
#   - Docker build cache unused for 24 h
#   - worktrees whose work is on origin/main, with nothing uncommitted
#   - build-*/ dirs in the main checkout untouched for --days days
#
# Refuses to remove anything while a container of ours is running: that is
# another agent's test, and its volume or image would go from under it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL=org.sonora=1
# Every image a script in tools/ builds or runs. Kept even when unused today.
# The shellcheck, actionlint and ubuntu images CI checks run in are not ours
# by label or name, so they are never candidates in the first place.
KEEP_IMAGES=(sonora-build:trixie sonora-runtime:trixie sonora-ui:trixie
             sonora-livebuild:trixie sonora-usbtest:trixie sonora-bwf-readers:trixie)
CACHE_VOLUME=sonora-livebuild-work
# A worktree whose git index or HEAD moved this recently may be mid-task even
# if no agent locked it.
ACTIVE_MINUTES="${SONORA_CLEAN_ACTIVE_MINUTES:-120}"

DRY=0
DEEP=0
DAYS=3
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run|-n) DRY=1 ;;
    --deep)       DEEP=1 ;;
    --days)
      [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || { echo "error: --days needs a number" >&2; exit 2; }
      DAYS="$2"
      shift ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "error: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

# Git Bash rewrites arguments that look like Unix paths. Docker gets none
# here, but a filter or template value must reach it verbatim. Not exported:
# git, du and find need the rewriting for this checkout's own paths.
docker() { MSYS2_ARG_CONV_EXCL='*' command docker "$@"; }

# --- sizes ---
human() {
  awk -v b="$1" 'BEGIN {
    split("B kB MB GB TB", u, " "); i = 1
    while (b >= 1000 && i < 5) { b /= 1000; i++ }
    printf (i == 1 ? "%d%s" : "%.1f%s"), b, u[i] }'
}
# Docker prints sizes as 1.5GB, 12.3kB, 0B (powers of 1000).
to_bytes() {
  awk -v s="$1" 'BEGIN {
    n = s + 0; m = 1
    if (s ~ /kB$/) m = 1e3; else if (s ~ /MB$/) m = 1e6
    else if (s ~ /GB$/) m = 1e9; else if (s ~ /TB$/) m = 1e12
    printf "%.0f", n * m }'
}
du_bytes() { du -sk "$1" 2>/dev/null | awk '{ printf "%.0f", $1 * 1024 }'; }

RECLAIMED=0
FAILURES=0
# act BYTES WHAT COMMAND... -- remove one thing, or say it would.
act() {
  local bytes="$1" what="$2" out
  shift 2
  if [[ "$DRY" == 1 ]]; then
    printf '  would remove %9s  %s\n' "$(human "$bytes")" "$what"
    RECLAIMED=$((RECLAIMED + bytes))
  elif out="$("$@" 2>&1 </dev/null)"; then
    printf '  removed      %9s  %s\n' "$(human "$bytes")" "$what"
    RECLAIMED=$((RECLAIMED + bytes))
  else
    printf '  FAILED       %9s  %s (left in place): %s\n' "$(human "$bytes")" "$what" "$(head -1 <<<"$out")"
    FAILURES=$((FAILURES + 1))
  fi
}
keep() { printf '  keep                    %s (%s)\n' "$1" "$2"; }
in_list() {
  local x="$1"
  shift
  local y
  for y in "$@"; do [[ "$x" == "$y" ]] && return 0; done
  return 1
}

[[ "$DRY" == 1 ]] && echo "DRY RUN: nothing is removed."

# --- Docker ---
DOCKER=0
if command -v docker >/dev/null && docker info >/dev/null 2>&1; then
  DOCKER=1
else
  echo "docker: not reachable, Docker cleanup skipped."
fi

if [[ "$DOCKER" == 1 ]]; then
  # Running containers of ours: by label, or by an image or name of ours.
  labelled="$(docker ps -q --filter "label=$LABEL")"
  running=""
  while read -r id image name status; do
    if [[ "$image" == sonora-* || "$name" == sonora-* ]] || grep -qx "$id" <<<"$labelled"; then
      running+="  $name ($image) $status"$'\n'
    fi
  done < <(docker ps --format '{{.ID}} {{.Image}} {{.Names}} {{.Status}}')
  if [[ -n "$running" ]]; then
    echo "Containers of ours are running (another agent's test):"
    printf '%s' "$running"
    if [[ "$DRY" != 1 ]]; then
      echo "Refusing to clean while they run. Wait for them to finish, then run again."
      exit 1
    fi
    echo "(a real run would refuse now)"
  fi

  echo "docker system df before:"
  docker system df | sed 's/^/  /'

  echo "Stopped containers of ours:"
  labelled="$(docker ps -aq --filter "label=$LABEL")"
  found=0
  while read -r id image name size; do
    [[ -n "$id" ]] || continue
    if [[ "$image" == sonora-* || "$name" == sonora-* ]] || grep -qx "$id" <<<"$labelled"; then
      found=1
      act "$(to_bytes "${size%% *}")" "container $name ($image)" docker rm "$id"
    fi
  done < <(docker ps -a -s --filter status=exited --filter status=created --filter status=dead \
             --format '{{.ID}} {{.Image}} {{.Names}} {{.Size}}')
  [[ "$found" == 1 ]] || echo "  none"

  echo "Dangling images of ours:"
  found=0
  others=0
  labelled="$(docker images -q --no-trunc --filter dangling=true --filter "label=$LABEL")"
  for id in $(docker images -q --no-trunc --filter dangling=true | sort -u); do
    # Rebuilds from before images were labelled left untagged ones with no
    # label; those are ours if a step of our Dockerfiles built them.
    if grep -qx "$id" <<<"$labelled" || docker history --no-trunc "$id" 2>/dev/null | grep -qi sonora; then
      found=1
      act "$(docker image inspect -f '{{.Size}}' "$id")" "image ${id#sha256:}" docker rmi "$id"
    else
      others=$((others + 1))
    fi
  done
  [[ "$found" == 1 ]] || echo "  none"
  [[ "$others" == 0 ]] || echo "  ($others dangling image(s) not ours, left alone)"

  echo "Images of ours that no tools/ script uses:"
  found=0
  labelled="$(docker images -q --filter "label=$LABEL" | sort -u)"
  kept_ids="$(for ref in "${KEEP_IMAGES[@]}"; do docker images -q "$ref"; done | sort -u)"
  while read -r id ref; do
    [[ "$ref" != "<none>:<none>" ]] || continue
    [[ "$ref" == sonora-* ]] || grep -qx "$id" <<<"$labelled" || continue
    if in_list "$ref" "${KEEP_IMAGES[@]}"; then
      keep "image $ref" "used by tools/"
      continue
    fi
    # A second tag on a kept image frees nothing when removed; it is untagged.
    if grep -qx "$id" <<<"$kept_ids"; then bytes=0; else bytes="$(docker image inspect -f '{{.Size}}' "$id")"; fi
    found=1
    act "$bytes" "image $ref" docker rmi "$ref"
  done < <(docker images --format '{{.ID}} {{.Repository}}:{{.Tag}}')
  [[ "$found" == 1 ]] || echo "  none to remove"

  echo "Volumes of ours:"
  found=0
  sizes="$(docker system df -v | awk '/^Local Volumes space usage/ { v = 1; next }
                                     /^Build cache/ { v = 0 }
                                     v && NF == 3 && $1 != "VOLUME" { print $1, $3 }')"
  labelled="$(docker volume ls -q --filter "label=$LABEL")"
  for vol in $(docker volume ls -q); do
    [[ "$vol" == sonora-* ]] || grep -qx "$vol" <<<"$labelled" || continue
    size="$(awk -v n="$vol" '$1 == n { print $2 }' <<<"$sizes")"
    bytes="$(to_bytes "${size:-0B}")"
    if [[ "$vol" == "$CACHE_VOLUME" && "$DEEP" != 1 ]]; then
      keep "volume $vol, $(human "$bytes")" "live-build cache; --deep removes it"
      continue
    fi
    found=1
    act "$bytes" "volume $vol" docker volume rm "$vol"
  done
  [[ "$found" == 1 ]] || echo "  none to remove"

  echo "Build cache unused for 24 h:"
  if [[ "$DRY" == 1 ]]; then
    total="$(docker system df --format '{{.Type}} {{.Reclaimable}}' | awk '/^Build Cache/ { print $3 }')"
    echo "  would prune   the part of ${total:-0B} reclaimable not used in 24 h (docker cannot size it in advance)"
  else
    pruned="$(docker builder prune -f --filter until=24h 2>&1 | awk '/^Total/ { print $NF }')"
    bytes="$(to_bytes "${pruned:-0B}")"
    printf '  removed      %9s  build cache\n' "$(human "$bytes")"
    RECLAIMED=$((RECLAIMED + bytes))
  fi
fi

# --- worktrees ---
MAIN="$(git -C "$ROOT" worktree list --porcelain | sed -n '1s/^worktree //p')"
SELF="$(git -C "$ROOT" rev-parse --show-toplevel)"
if ! git -C "$MAIN" fetch -q --prune origin 2>/dev/null; then
  echo "warning: git fetch failed; judging worktrees against the last fetched origin/main."
fi
HAVE_GH=0
command -v gh >/dev/null && gh auth status >/dev/null 2>&1 && HAVE_GH=1

# merged_into_main DIR BRANCH -- is every commit of the worktree on origin/main?
merged_into_main() {
  local dir="$1" branch="$2" base files
  git -C "$dir" merge-base --is-ancestor HEAD origin/main && return 0
  # Squash merges leave no common history. The work is on main if every file
  # the branch changed reads the same there now. A later change on main to the
  # same files makes this say no, which keeps the worktree: the safe side.
  base="$(git -C "$dir" merge-base HEAD origin/main)" || return 1
  mapfile -t files < <(git -C "$dir" diff --name-only "$base" HEAD)
  if [[ "${#files[@]}" -gt 0 ]] && git -C "$dir" diff --quiet HEAD origin/main -- "${files[@]}"; then
    return 0
  fi
  # Or GitHub says a PR from exactly this commit was merged.
  [[ "$HAVE_GH" == 1 && "$branch" != "(detached)" ]] || return 1
  gh pr list --state merged --head "$branch" --json headRefOid -q '.[].headRefOid' </dev/null 2>/dev/null \
    | grep -qx "$(git -C "$dir" rev-parse HEAD)"
}

judge_worktree() {
  local wt="$1" branch="$2" locked="$3" gitdir ahead
  [[ "$wt" == "$MAIN/.claude/worktrees/"* ]] || return 0
  if [[ "$wt" == "$SELF" ]]; then keep "worktree $wt" "this script runs from it"; return; fi
  if [[ ! -d "$wt" ]]; then
    act 0 "stale worktree entry $wt (directory gone)" git -C "$MAIN" worktree prune
    return
  fi
  if [[ "$locked" == 1 ]]; then keep "worktree $wt" "locked: an agent is using it"; return; fi
  if [[ -n "$(git -C "$wt" status --porcelain 2>/dev/null)" ]]; then
    keep "worktree $wt [$branch]" "LIST: uncommitted changes"
    return
  fi
  gitdir="$(git -C "$wt" rev-parse --absolute-git-dir)"
  if [[ -n "$(find "$gitdir" -maxdepth 1 \( -name index -o -name HEAD \) -mmin "-$ACTIVE_MINUTES" 2>/dev/null)" ]]; then
    keep "worktree $wt [$branch]" "active in the last $ACTIVE_MINUTES min"
    return
  fi
  if ! merged_into_main "$wt" "$branch"; then
    ahead="$(git -C "$wt" rev-list --count origin/main..HEAD 2>/dev/null || echo '?')"
    keep "worktree $wt [$branch]" "LIST: $ahead commit(s) not on origin/main"
    return
  fi
  act "$(du_bytes "$wt")" "worktree $wt [$branch], merged" git -C "$MAIN" worktree remove "$wt"
}

echo "Worktrees under .claude/worktrees:"
wt=""; branch=""; locked=0
while IFS= read -r line; do
  case "$line" in
    "worktree "*) wt="${line#worktree }"; branch="(detached)"; locked=0 ;;
    "branch refs/heads/"*) branch="${line#branch refs/heads/}" ;;
    locked*) locked=1 ;;
    "") [[ -z "$wt" ]] || judge_worktree "$wt" "$branch" "$locked"; wt="" ;;
  esac
done < <(git -C "$MAIN" worktree list --porcelain; echo)

# --- build output in the main checkout ---
echo "build-*/ in the main checkout untouched for $DAYS day(s):"
found=0
for d in "$MAIN"/build-*/; do
  [[ -d "$d" ]] || continue
  d="${d%/}"
  if [[ -n "$(find "$d" -mtime "-$DAYS" -print -quit 2>/dev/null)" ]]; then
    keep "$d" "used in the last $DAYS day(s)"
    continue
  fi
  found=1
  act "$(du_bytes "$d")" "$d" rm -rf "$d"
done
[[ "$found" == 1 ]] || echo "  none to remove"

echo
if [[ "$DRY" == 1 ]]; then
  echo "SUMMARY would reclaim $(human "$RECLAIMED") (plus build cache older than 24 h)"
else
  if [[ "$DOCKER" == 1 ]]; then
    echo "docker system df after:"
    docker system df | sed 's/^/  /'
  fi
  echo "SUMMARY reclaimed $(human "$RECLAIMED"), $FAILURES failure(s)"
  echo "Docker Desktop's disk file does not shrink by itself; compacting it is an owner step (.claude/agents/janitor.md)."
fi
[[ "$FAILURES" == 0 ]]
