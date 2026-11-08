/* The conversation: five message kinds, cards, scroll behaviour, composer. */
"use strict";
let lastSig = "";
let notice = "";

function showNotice(text) { notice = text; lastSig = ""; if (C.data) renderChat(); }

function bubble(m) {
  const t = hhmm(m.ts), body = `<p>${esc(m.text)}</p>`;
  if (m.kind === "system") return `<div class="sys">${esc(m.text)}</div>`;
  if (m.kind === "user") {
    const to = m.to ? ` → ${esc(m.to)}` : "";
    return `<div class="msg me"><div class="who"><span>Você${to}</span><time>${t}</time></div>${body}</div>`;
  }
  if (m.kind === "agent") return `<div class="msg agm"><div class="who"><span>${esc(m.from)}</span><time>${t}</time></div>${body}</div>`;
  if (m.kind === "alert") return `<div class="msg alert"><div class="who"><span>▲ Alerta</span><time>${t}</time></div>${body}</div>`;
  return `<div class="msg"><div class="who"><span>Orquestrador</span><time>${t}</time></div>${body}</div>`;
}

function thinking(messages) {
  const open = messages.filter(m => m.kind === "user" && !m.to);
  if (!open.length) return false;
  const answered = new Set(messages.filter(m => m.kind === "orch" || m.kind === "system").map(m => m.req));
  return open.some(m => !answered.has(m.id));
}

function welcome() {
  return '<div class="msg"><div class="who"><span>Orquestrador</span></div>' +
    "<p>Oi. Pergunte como estão as coisas ou peça uma ação; nada roda sem o seu Confirmar.</p></div>";
}

function renderChat() {
  const chat = C.data.chat, log = $("#log");
  const sig = JSON.stringify([chat, Object.keys(CLICKED), notice, C.queue.length]);
  if (sig === lastSig) return;
  lastSig = sig;
  const atEnd = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
  const prev = log.querySelectorAll(".msg,.card,.sys").length;
  let html = chat.messages.length ? "" : welcome();
  html += chat.messages.map(m => bubble(m) + cardsHtml(m, chat.actions)).join("");
  html += C.queue.map(q => `<div class="msg me"><div class="who"><span>Você${q.to ? " → " + esc(q.to) : ""}</span>` +
    `<time>enviando…</time></div><p>${esc(q.text)}</p></div>`).join("");
  if (thinking(chat.messages)) html += '<div class="msg think"><div class="who"><span>Orquestrador</span></div><p><i></i><i></i><i></i></p></div>';
  if (notice) html += `<div class="sys">${esc(notice)}</div>`;
  log.innerHTML = html;
  wireCards(log);
  const now = log.querySelectorAll(".msg,.card,.sys").length;
  if (atEnd) { log.scrollTop = log.scrollHeight; $("#newer").hidden = true; }
  else if (now > prev) $("#newer").hidden = false;
}

function initComposer() {
  $("#sug").innerHTML = SUGGESTIONS.map(s => `<button type="button">${esc(s)}</button>`).join("");
  document.querySelectorAll("#sug button").forEach(b => b.onclick = () => submit(b.textContent));
  $("#form").onsubmit = e => { e.preventDefault(); submit($("#inp").value); $("#inp").value = ""; };
  $("#newer").onclick = () => { $("#log").scrollTop = $("#log").scrollHeight; $("#newer").hidden = true; };
}

function submit(text) { notice = ""; send(text, C.target); }

/* ?pane=agentes|atencao and ?to=<agent> open the screen in that state (links, screenshots). */
const QUERY = new URLSearchParams(location.search);
if (QUERY.get("to")) C.target = QUERY.get("to");
if (QUERY.get("pane")) document.body.dataset.pane = QUERY.get("pane");
initComposer();
renderTo();
poll();
setInterval(poll, POLL_MS);
