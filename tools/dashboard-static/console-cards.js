/* Cards in the conversation: the action card and its state machine
   (pending -> running -> done | failed; pending -> cancelled | expired)
   and the read-only list card. Buttons only post a decision; the daemon
   runs the action. */
"use strict";
const LABEL = {pending: ["AGUARDA", "pend"], running: ["EXECUTANDO", "run"], done: ["EXECUTADO", "done"],
  failed: ["FALHOU", "fail"], cancelled: ["CANCELADO", "undo"], expired: ["EXPIRADO", "exp"]};
const CLICKED = {};   /* action id -> true between the tap and the daemon's answer */

function stateOf(a) {
  if (a.state !== "pending") { delete CLICKED[a.id]; return a.state; }
  return CLICKED[a.id] ? "running" : "pending";
}

function confirmLabel(a) {
  return a.risk ? a.title : "Confirmar";
}

function footer(a, st) {
  if (st === "pending") {
    return `<button class="btn go" data-act="confirm" data-id="${esc(a.id)}">${esc(confirmLabel(a))}</button>` +
      `<button class="btn" data-act="cancel" data-id="${esc(a.id)}">Cancelar</button>` +
      '<span class="info">Nada roda até confirmar</span>';
  }
  if (st === "failed") {
    return `<button class="btn" data-act="retry" data-id="${esc(a.id)}">Tentar de novo</button>` +
      `<span class="info">${esc(a.result)}</span>`;
  }
  const info = {running: "enviado ao daemon", done: esc(a.result), cancelled: "nada foi executado",
    expired: "passou de 15 min sem resposta"}[st] || "";
  return `<span class="info" style="margin-left:0">${info}</span>`;
}

function actionCard(a) {
  const st = stateOf(a), [text, cls] = LABEL[st] || LABEL.pending;
  const steps = (a.steps || []).map(s => `<li>${esc(s)}</li>`).join("");
  return `<div class="card ${a.risk ? "risk" : ""} ${st === "pending" || st === "running" ? "" : st}">
    <header><span class="k">${a.risk ? "▲ Ação com risco" : "Ação proposta"}</span><span class="badge ${cls}">${text}</span></header>
    <div class="body"><h5>${esc(a.title)}</h5><ul class="steps">${steps}</ul><div class="effects">${esc(a.effect)}</div></div>
    <footer>${footer(a, st)}</footer></div>`;
}

function listRow(r) {
  const row = typeof r === "string" ? {text: r} : r;
  const cells = row.id ? `<td>${esc(row.id)}</td><td>${esc(row.text)}</td>` : `<td>${esc(row.text)}</td>`;
  return `<tr${row.id ? ` data-fill="${esc(row.id)}"` : ""}>${cells}</tr>`;
}

function listCard(c) {
  return `<div class="card done"><header><span class="k">${esc(c.title || "Lista")}</span>` +
    `<span class="badge run">SÓ LEITURA</span></header><div class="body"><table class="tbl">` +
    `${(c.rows || []).map(listRow).join("")}</table></div></div>`;
}

function cardsHtml(m, actions) {
  return (m.cards || []).map(c => {
    if (c.type === "list") return listCard(c);
    const a = actions[c.id];
    return a ? actionCard(a) : "";
  }).join("");
}

async function decide(kind, id) {
  if (kind === "confirm") CLICKED[id] = true;   /* a second tap cannot repeat it */
  try { await post(`/api/action/${encodeURIComponent(id)}/${kind}`); }
  catch (e) { delete CLICKED[id]; showNotice(e.message); }
  poll();
}

function wireCards(root) {
  root.querySelectorAll("[data-act]").forEach(b => b.onclick = () => decide(b.dataset.act, b.dataset.id));
  root.querySelectorAll("tr[data-fill]").forEach(r => r.onclick = () => {
    $("#inp").value = r.dataset.fill + " "; $("#inp").focus();
  });
}
