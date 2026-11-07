#!/usr/bin/env bash
# The name of the micro build jobs, from the changed paths on stdin
# (.github/workflows/ci.yml, `changes` job): what the job does, readable in
# the PR's check list. The path table is micro-build.sh's.
#
#   printf '%s\n' "$files" | tools/ci-micro-label.sh build|rtsan
#
# build: "Build + test: ui, model (12 files)"; rtsan: the real-time paths
# only, "RTSan: engine-core (3 files)"; neither applies: "Nothing to build
# (docs/harness only)".
set -uo pipefail
mode="${1:-build}"
ctx=() n=0
add() { local c; for c in "${ctx[@]:-}"; do [[ "$c" != "$1" ]] || return 0; done; ctx+=("$1"); }
while IFS= read -r f || [[ -n "$f" ]]; do
  [[ -n "$f" ]] || continue
  name="" rt=0
  case "$f" in
    ui/*|keymaps/*) name=ui ;;
    engine/*/*) name="engine-$(cut -d/ -f2 <<<"$f")"; rt=1 ;;
    engine/*|drivers/*) name="${f%%/*}"; rt=1 ;;
    plugins/client/*|plugins/native/*|plugins/sdk/*) name=plugins-native; rt=1 ;;
    common/dsp/*) name=dsp ;;
    common/*/*) name="$(cut -d/ -f2 <<<"$f")"; rt=1 ;;
    contracts/*) name=contracts; rt=1 ;;
    plugins/*) name=plugins ;;
    interchange/*) name=interchange ;;
    CMakeLists.txt|cmake/*|tools/*.cmake) name=whole-tree ;;
    *) continue ;;
  esac
  if [[ "$mode" == rtsan && $rt == 0 ]]; then continue; fi
  add "$name"; n=$((n + 1))
done
if ((n == 0)); then
  [[ "$mode" == rtsan ]] && echo "RTSan: no real-time code changed" || echo "Nothing to build (docs/harness only)"
  exit 0
fi
shown=$(IFS=,; echo "${ctx[*]:0:4}" | sed 's/,/, /g')
((${#ctx[@]} <= 4)) || shown="$shown +$((${#ctx[@]} - 4))"
[[ "$mode" == rtsan ]] && head="RTSan" || head="Build + test"
echo "$head: $shown ($n files)"
