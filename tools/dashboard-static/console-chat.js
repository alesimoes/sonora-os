/* The conversation: five message kinds, cards, scroll behaviour, composer. */
"use strict";
let lastSig = "";
let notice = "";

function showNotice(text) { notice = text; lastSig = ""; if (C.data) renderChat(); }

/* A long note (an agent's report runs to 900 characters) shows its start and
   opens on a tap, so one message does not fill the screen (owner, 2026-11-09).
   <details> keeps its open state across redraws of other items. */
const LONG_TEXT = 420;
function bodyHtml(text) {
  text = String(text || "");
  if (text.length <= LONG_TEXT) return `<p>${esc(text)}</p>`;
  return `<details class="more"><summary><p>${esc(text.slice(0, 280))}… <u>ver tudo</u></p></summary>` +
    `<p>${esc(text)}</p></details>`;
}

function bubble(m) {
  const t = hhmm(m.ts), body = m.kind === "user" ? `<p>${esc(m.text)}</p>` : bodyHtml(m.text);
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

/* "pensando" at once; the half-written answer shows in it when the agent sent one. */
function thinkingBubble(partial) {
  const text = partial && partial.text ? `<span class="partial">${esc(partial.text)} </span>` : "";
  return `<div class="msg think"><div class="who"><span>Orquestrador</span></div><p>${text}<i></i><i></i><i></i></p></div>`;
}

function welcome() {
  return '<div class="msg"><div class="who"><span>Orquestrador</span></div>' +
    "<p>Oi. Pergunte como estão as coisas ou peça uma ação; nada roda sem o seu Confirmar.</p></div>";
}

function renderChat() {
  const chat = C.data.chat, log = $("#log");
  const echo = pendingEcho(chat.messages);
  const messages = chat.messages.concat(echo);
  const sig = JSON.stringify([chat, Object.keys(CLICKED), notice, C.queue.length, echo.length]);
  if (sig === lastSig) return;
  lastSig = sig;
  const atEnd = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
  const top = log.scrollTop;
  const items = [];
  if (!messages.length) items.push(["welcome", welcome()]);
  messages.forEach((m, i) => items.push(["m:" + (m.id || i), bubble(m) + cardsHtml(m, chat.actions)]));
  C.queue.forEach((q, i) => items.push(["q:" + i, `<div class="msg me"><div class="who"><span>Você${q.to ? " → " + esc(q.to) : ""}</span>` +
    `<time>enviando…</time></div><p>${esc(q.text)}</p></div>`]));
  if (thinking(messages)) items.push(["think", thinkingBubble(chat.partial)]);
  if (notice) items.push(["notice", `<div class="sys">${esc(notice)}</div>`]);
  const added = patchLog(log, items);
  if (atEnd) { log.scrollTop = log.scrollHeight; $("#newer").hidden = true; }
  else { log.scrollTop = top; if (added) $("#newer").hidden = false; }
}

/* Only the items whose markup changed are replaced, new ones appended, gone
   ones removed: rewriting the whole log on every pushed frame made the chat
   shake while the owner scrolled (2026-11-08). Each item is a display:contents
   wrapper, so the log's layout is the same as before. Returns how many were new. */
function patchLog(log, items) {
  const old = new Map();
  for (const el of Array.from(log.children)) {
    if (el.dataset && el.dataset.k) old.set(el.dataset.k, el); else el.remove();
  }
  let added = 0, prev = null;
  for (const [k, html] of items) {
    let el = old.get(k);
    if (el) old.delete(k);
    if (!el || el._html !== html) {
      const fresh = document.createElement("div");
      fresh.className = "it"; fresh.dataset.k = k; fresh._html = html; fresh.innerHTML = html;
      wireCards(fresh);
      if (el) el.replaceWith(fresh); else added++;
      el = fresh;
    }
    const want = prev ? prev.nextSibling : log.firstChild;
    if (want !== el) log.insertBefore(el, want);
    prev = el;
  }
  old.forEach(el => el.remove());
  return added;
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
startStream();
setInterval(() => { if (!C.live) poll(); }, POLL_MS);
