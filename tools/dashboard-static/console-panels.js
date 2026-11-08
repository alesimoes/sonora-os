/* Status strip, agent list, attention queue, tabs, composer target. */
"use strict";
/* Replace a panel only when its markup changed: rewriting it every 3 s poll
   restarted animations and made the console blink (owner, 2026-11-08). */
function setHTML(el, html) {
  if (el._html === html) return false;
  el._html = html;
  el.innerHTML = html;
  return true;
}

function cell(label, c, extra) {
  const bar = c.cap ? `<div class="bar"><span style="width:${Math.min(100, c.n / c.cap * 100)}%"></span></div>` : "";
  const val = c.cap ? `${c.n}<em>/${c.cap}</em>` : esc(c.text);
  return `<div class="cell ${c.level} ${extra || ""}"><small>${label}</small><b>${val}</b>${bar}</div>`;
}

function renderStrip() {
  const s = C.data.strip, d = s.daemon;
  const dot = d.level === "bad" ? "" : '<span class="live"><span class="dot"></span></span>';
  const dcell = `<div class="cell ${d.level}"><small>Daemon</small><b>${dot}${esc(d.text)}</b></div>`;
  const strip = $("#strip");
  if (!strip.querySelector(".cells"))
    strip.innerHTML = '<div class="brand live"><i></i><span>Sonora Console</span></div><div class="cells"></div>';
  if (!setHTML(strip.querySelector(".cells"), dcell + cell("Agentes", s.agents) + cell("Execuções", s.runs, "hide-s") +
    cell("PRs", s.prs) + `<div class="cell ${s.alerts.level}"><small>Alertas</small><b>${s.alerts.n}</b></div>`)) return;
  document.querySelectorAll("#strip .cell").forEach((el, i) => {
    const q = [null, "quem está rodando?", "o que o freio está segurando?", "quais PRs estão abertos?", "o que precisa de mim?"][i];
    if (q) el.onclick = () => { $("#inp").value = q; };
  });
}

function agentCard(a) {
  const sel = C.target === a.id;
  const quiet = a.no_signal_min != null;
  return `<button class="ag ${quiet ? "bad" : ""} ${sel ? "sel" : ""}" data-id="${esc(a.id)}" aria-pressed="${sel}">
    <div class="n"><span>${esc(a.id)}</span><span>${esc(a.model)}</span></div>
    <div class="t">${esc(a.task)}</div>
    <div class="m"><b>${a.minutes == null ? "?" : a.minutes} min</b><span>${esc(a.role)}</span>` +
    `${a.pr ? `<span>#${esc(a.pr)}</span>` : ""}${quiet ? `<span class="st">sem sinal ${a.no_signal_min} min</span>` : ""}</div></button>`;
}

function renderAgents() {
  const list = C.data.agents, cap = C.data.strip.agents.cap;
  $("#agcount").textContent = list.length + (cap ? " / " + cap : "");
  if (!setHTML($("#agents"), (list.length ? list.map(agentCard).join("") : '<div class="empty">Nenhum agente rodando.</div>') +
    `<button class="ag all" data-id="">${C.target ? "↩ voltar ao orquestrador" : "Toque num agente para falar com ele"}</button>`)) return;
  document.querySelectorAll(".ag[data-id]").forEach(b => b.onclick = () => {
    setTarget(b.dataset.id || null);
    if (innerWidth <= 900) pane("chat");
    $("#inp").focus();
  });
}

function renderAttention() {
  const q = C.data.attention;
  $("#atcount").textContent = q.length;
  if (!setHTML($("#attn"), q.length ? q.map((x, i) => `<div class="at ${x.level === "bad" ? "bad" : ""}">
    <h4>${esc(x.title)}</h4><p>${esc(x.reason)}</p>` +
    '<div class="at-act">' +
    (x.button ? `<button class="btn sm ${x.level === "bad" ? "am" : ""}" data-i="${i}">${esc(x.button.label)}</button>` : "") +
    `<button class="btn sm cp" data-c="${i}">Copiar</button></div>` +
    "</div>").join("") : '<div class="empty">Nada precisa de você agora.</div>')) return;
  document.querySelectorAll(".at .cp").forEach(b => b.onclick = () => copyText(b, q[b.dataset.c]));
  document.querySelectorAll(".at .btn[data-i]").forEach(b => b.onclick = () => {
    setTarget(null); pane("chat"); send(q[b.dataset.i].button.command);
  });
}

function renderTabs() {
  const n = C.data.agents.length, a = C.data.attention.length, cur = document.body.dataset.pane;
  if (!setHTML($("#tabs"), [["chat", "Chat"], ["agentes", "Agentes", n], ["atencao", "Atenção", a]].map(([k, l, c]) =>
    `<button data-p="${k}" class="${cur === k ? "on" : ""} ${k === "atencao" && c ? "al" : ""}">${l}${c != null ? `<i>${c}</i>` : ""}</button>`).join(""))) return;
  document.querySelectorAll("#tabs button").forEach(b => b.onclick = () => pane(b.dataset.p));
}

function pane(p) { document.body.dataset.pane = p; if (C.data) renderTabs(); }

function setTarget(id) { C.target = id; if (C.data) renderAgents(); renderTo(); }

function renderTo() {
  $("#to").innerHTML = C.target
    ? `<span>Para</span><span class="pill pa">${esc(C.target)}</span><button type="button" id="back">voltar ao orquestrador</button>`
    : '<span>Para</span><span class="pill">orquestrador</span>';
  if (C.target) $("#back").onclick = () => setTarget(null);
  $("#inp").placeholder = C.target ? `Mensagem direta para ${C.target}…` : "Escreva um pedido… ex.: pare o eng-104";
}

/* The owner pastes an attention item elsewhere (owner, 2026-11-08). The
   clipboard API needs a secure context; the textarea copy covers the rest. */
function copyText(btn, x) {
  const text = x.title + "\n" + x.reason;
  const done = () => { btn.textContent = "Copiado"; setTimeout(() => { btn.textContent = "Copiar"; }, 1500); };
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(done, () => fallbackCopy(text, done));
  } else fallbackCopy(text, done);
}

function fallbackCopy(text, done) {
  const t = document.createElement("textarea");
  t.value = text; t.style.position = "fixed"; t.style.opacity = "0";
  document.body.appendChild(t); t.select();
  try { document.execCommand("copy"); done(); } catch (e) { /* text stays selectable */ }
  t.remove();
}
