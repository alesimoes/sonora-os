/* Status strip, agent list, attention queue, tabs, composer target. */
"use strict";
function cell(label, c, extra) {
  const bar = c.cap ? `<div class="bar"><span style="width:${Math.min(100, c.n / c.cap * 100)}%"></span></div>` : "";
  const val = c.cap ? `${c.n}<em>/${c.cap}</em>` : esc(c.text);
  return `<div class="cell ${c.level} ${extra || ""}"><small>${label}</small><b>${val}</b>${bar}</div>`;
}

function renderStrip() {
  const s = C.data.strip, d = s.daemon;
  const dot = d.level === "bad" ? "" : '<span class="live"><span class="dot"></span></span>';
  const dcell = `<div class="cell ${d.level}"><small>Daemon</small><b>${dot}${esc(d.text)}</b></div>`;
  $("#strip").innerHTML = '<div class="brand live"><i></i><span>Sonora Console</span></div><div class="cells">' +
    dcell + cell("Agentes", s.agents) + cell("Execuções", s.runs, "hide-s") + cell("PRs", s.prs) +
    `<div class="cell ${s.alerts.level}"><small>Alertas</small><b>${s.alerts.n}</b></div></div>`;
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
  $("#agents").innerHTML = (list.length ? list.map(agentCard).join("") : '<div class="empty">Nenhum agente rodando.</div>') +
    `<button class="ag all" data-id="">${C.target ? "↩ voltar ao orquestrador" : "Toque num agente para falar com ele"}</button>`;
  document.querySelectorAll(".ag[data-id]").forEach(b => b.onclick = () => {
    setTarget(b.dataset.id || null);
    if (innerWidth <= 900) pane("chat");
    $("#inp").focus();
  });
}

function renderAttention() {
  renderHarness();
  const q = C.data.attention;
  $("#atcount").textContent = q.length;
  $("#attn").innerHTML = q.length ? q.map((x, i) => `<div class="at ${x.level === "bad" ? "bad" : ""}">
    <h4>${esc(x.title)}</h4><p>${esc(x.reason)}</p>` +
    (x.button ? `<button class="btn sm ${x.level === "bad" ? "am" : ""}" data-i="${i}">${esc(x.button.label)}</button>` : "") +
    "</div>").join("") : '<div class="empty">Nada precisa de você agora.</div>';
  document.querySelectorAll(".at .btn").forEach(b => b.onclick = () => {
    setTarget(null); pane("chat"); send(q[b.dataset.i].button.command);
  });
}

function renderHarness() {
  const h = C.data.harness || {n: 0, groups: []};
  const open = document.querySelector("#hcare details")?.open;
  $("#hcare").innerHTML = h.n ? `<details${open ? " open" : ""}><summary>harness cuidando <b>${h.n}</b></summary>` +
    h.groups.map(g => `<div class="hc"><h4>${esc(g.title)}</h4><p>${esc(g.reason)}</p></div>`).join("") + "</details>" : "";
}

function renderTabs() {
  const n = C.data.agents.length, a = C.data.attention.length, cur = document.body.dataset.pane;
  $("#tabs").innerHTML = [["chat", "Chat"], ["agentes", "Agentes", n], ["atencao", "Atenção", a]].map(([k, l, c]) =>
    `<button data-p="${k}" class="${cur === k ? "on" : ""} ${k === "atencao" && c ? "al" : ""}">${l}${c != null ? `<i>${c}</i>` : ""}</button>`).join("");
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
