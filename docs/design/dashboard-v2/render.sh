#!/usr/bin/env bash
# Renderiza cada vista em PNG com Edge/Chrome headless. Uso: render.sh [caminho-do-navegador]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../../images/dashboard-v2"
br="${1:-${BROWSER:-C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe}}"
url="file:///$(cd "$here" && pwd -W 2>/dev/null || pwd)/index.html"
mkdir -p "$out"
shot() { "$br" --headless=new --disable-gpu --hide-scrollbars --window-size="$3" --screenshot="$(cd "$out" && (pwd -W 2>/dev/null || pwd))/$1.png" "${url%index.html}${5:-index.html}?tema=$2#$4" >/dev/null 2>&1; }
shot_phone() { shot "$1" "$2" 520,844 "$3" "celular.html"; }
for v in visao agentes prs freios progresso; do
  shot "$v-escuro" escuro 1920,1080 "$v"
  shot "$v-claro" claro 1920,1080 "$v"
  shot_phone "$v-celular" escuro "$v"
done
