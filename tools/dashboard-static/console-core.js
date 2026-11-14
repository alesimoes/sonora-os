/* Sonora Console: shared state, helpers, polling and the one POST helper.
   Every string that came from the server goes through esc() before innerHTML. */
"use strict";
const $ = s => document.querySelector(s);
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g,
  c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const TOKEN = (document.querySelector('meta[name="dash-token"]') || {}).content || "";
const POLL_MS = 3000;
const STREAM_RETRY_MS = 5000;
const SUGGESTIONS = ["status", "o que precisa de mim?", "quais PRs estão abertos?",
  "por que não lança agente?", "quem está rodando?", "ajuda"];
const C = {data: null, target: null, online: true, queue: [], busy: false, echo: [], live: false};
const ECHO_MAX_MS = 60000;

function hhmm(iso) {
  const d = new Date(iso);
  return isNaN(d) ? "" : d.toLocaleTimeString("pt-BR", {hour: "2-digit", minute: "2-digit"});
}

async function post(path, body) {
  const r = await fetch(path, {method: "POST", body: JSON.stringify(body || {}),
    headers: {"Content-Type": "application/json", "X-Dash-Token": TOKEN}});
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(j.error || r.statusText), {status: r.status});
  return j;
}

async function poll() {
  try {
    const r = await fetch("/api/console", {cache: "no-store"});
    C.data = await r.json();
    setOnline(true);
    renderAll();
  } catch (e) {
    setOnline(false);
  }
}

/* Push: the server sends the whole console view when anything changes. While
   the stream is up the poll stays quiet; when it drops the poll takes over
   and the stream is retried. */
function startStream() {
  if (!window.EventSource) return;
  const es = new EventSource("/api/console/stream");
  es.addEventListener("console", e => {
    try { C.data = JSON.parse(e.data); } catch (err) { return; }
    C.live = true;
    setOnline(true);
    renderAll();
  });
  es.onerror = () => {
    C.live = false;
    es.close();
    setTimeout(startStream, STREAM_RETRY_MS);
  };
}

/* Local echoes the server has not shown yet (same text and target, or too old). */
function pendingEcho(messages) {
  const now = Date.now();
  C.echo = C.echo.filter(e => now - Date.parse(e.ts) < ECHO_MAX_MS &&
    !messages.some(m => m.kind === "user" && m.text === e.text && (m.to || null) === (e.to || null) &&
      Date.parse(m.ts) >= Date.parse(e.ts) - ECHO_MAX_MS));
  return C.echo;
}

function setOnline(on) {
  C.online = on;
  $("#offline").hidden = on;
  // Only when something waits: flushQueue ends with a poll, whose answer
  // calls setOnline again, so an unconditional flush polled back to back
  // forever (2026-11-09: ~1.4 requests a second per open page, the server
  // pinned a core, every answer took over a second).
  if (on && C.queue.length) flushQueue();
}

/* Messages wait here until the server takes them; a refusal (daemon down,
   bad input) is shown in the chat and the message is dropped, a network
   error keeps it for the next try. */
async function flushQueue() {
  if (C.busy) return;
  C.busy = true;
  while (C.queue.length) {
    try {
      const q = C.queue[0];
      await post("/api/chat", q);
      C.queue.shift();
      // Shown as sent at once and kept until the server's copy arrives, so a
      // push or poll that lands first does not make it vanish.
      C.echo.push({id: "local-" + Date.now(), kind: "user", text: q.text,
        to: q.to, ts: new Date().toISOString()});
      if (C.data && C.data.chat) renderChat();
    } catch (e) {
      if (!e.status) break;
      C.queue.shift();
      showNotice(e.message);
    }
  }
  C.busy = false;
  poll();
}

function send(text, to) {
  text = (text || "").trim();
  if (!text) return;
  C.queue.push({text, to: to || null});
  // The owner sees the message the moment it is sent (2026-11-08: it only
  // appeared on the next poll, seconds later, and looked lost).
  if (C.data) renderChat();
  flushQueue();
}

function renderAll() {
  renderStrip(); renderAgents(); renderNums(); renderAttention(); renderTabs(); renderChat();
}
