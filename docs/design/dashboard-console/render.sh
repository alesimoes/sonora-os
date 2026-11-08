#!/usr/bin/env bash
# Renderiza a maquete em PNG com Edge/Chrome headless. Uso: render.sh [caminho-do-navegador]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../../images/dashboard-console"
br="${1:-${BROWSER:-C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe}}"
mkdir -p "$out"
dir="file:///$(cd "$here" && (pwd -W 2>/dev/null || pwd))"
outw="$(cd "$out" && (pwd -W 2>/dev/null || pwd))"
shot() { "$br" --headless=new --disable-gpu --hide-scrollbars --window-size="$2" --screenshot="$outw/$1.png" "$dir/$4?$3" >/dev/null 2>&1; }
shot celular-chat 520,900 "pane=chat" celular.html
shot celular-agentes 520,900 "pane=agentes" celular.html
shot celular-atencao 520,900 "pane=atencao" celular.html
shot celular-agente-direto 520,900 "pane=chat&to=eng-101&direct=1" celular.html
shot celular-confirmado 520,900 "pane=chat&ok=1" celular.html
shot desktop-chat 1440,900 "pane=chat" index.html
shot desktop-agente-direto 1440,900 "pane=chat&to=eng-101&direct=1" index.html
