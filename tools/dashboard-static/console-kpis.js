/* The numbers at a glance: strip cells (progress, merges) and the compact
   panel (progress per track, open PRs, merges). Tapping a PR asks the chat. */
"use strict";

function kpiCells() {
  const k = C.data.kpis || {}, p = k.progress, m = k.merges;
  const pc = p ? `<div class="cell ok" data-q="quanto falta para terminar?"><small>Concluído</small><b>${p.pct}<em>%</em></b>` +
    `<div class="bar"><span style="width:${p.pct}%"></span></div></div>` :
    '<div class="cell" data-q="quanto falta para terminar?"><small>Concluído</small><b>—</b></div>';
  const mc = m ? `<div class="cell hide-s" data-q="o que entrou hoje?"><small>Merges hoje</small><b>${m.today}<em> · ${m.last_hour}/h</em></b></div>` : "";
  return pc + mc;
}

function trackRow(t) {
  const doing = t.doing ? `<em> +${t.doing} em curso</em>` : "";
  return `<div class="trk"><div class="n"><span>${esc(t.title)}</span><b>${t.pct}%</b></div>` +
    `<div class="bar"><span style="width:${t.pct}%"></span>` +
    `<span class="dg" style="left:${t.pct}%;width:${Math.min(100 - t.pct, t.total ? t.doing / t.total * 100 : 0)}%"></span></div>` +
    `<div class="m">${t.done}/${t.total} pts${doing}</div></div>`;
}

function renderProgress() {
  const p = (C.data.kpis || {}).progress, m = (C.data.kpis || {}).merges;
  const head = p ? `<div class="trk all"><div class="n"><span>Geral</span><b>${p.pct}%</b></div>` +
    `<div class="m">${p.done}/${p.total} pts${p.doing ? `<em> +${p.doing} em curso</em>` : ""}</div></div>` +
    p.tracks.map(trackRow).join("") : '<div class="empty">Progresso indisponível.</div>';
  const mg = m ? `<div class="mrg"><span>Merges hoje <b>${m.today}</b></span><span>última hora <b>${m.last_hour}</b></span></div>` : "";
  setHTML($("#prog"), head + mg);
}

function prRow(p) {
  const min = p.minutes_open == null ? "?" : p.minutes_open;
  return `<button class="pr ${p.level}" data-q="${esc(p.tap)}"><div class="n"><span>#${esc(p.number)}</span>` +
    `<span class="ci ${p.level}">${esc(p.ci)}</span></div><div class="t">${esc(p.title)}</div>` +
    `<div class="m"><span>${esc(p.train)}</span><b>${min} min</b></div></button>`;
}

function renderPrs() {
  const list = (C.data.kpis || {}).prs || [];
  $("#prcount").textContent = list.length;
  if (!setHTML($("#prlist"), list.length ? list.map(prRow).join("") : '<div class="empty">Nenhum PR aberto.</div>')) return;
  document.querySelectorAll("#prlist .pr").forEach(b => b.onclick = () => {
    setTarget(null); pane("chat"); send(b.dataset.q);
  });
}

function renderNums() { renderProgress(); renderPrs(); }
