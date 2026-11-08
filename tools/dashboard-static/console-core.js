/* Sonora Console: shared state, helpers, polling and the one POST helper.
   Every string that came from the server goes through esc() before innerHTML. */
"use strict";
const $ = s => document.querySelector(s);
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g,
  c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const TOKEN = (document.querySelector('meta[name="dash-token"]') || {}).content || "";
const POLL_MS = 3000;
const SUGGESTIONS = ["status", "o que precisa de mim?", "quais PRs estão abertos?",
  "por que não lança agente?", "quem está rodando?", "ajuda"];
const C = {data: null, target: null, online: true, queue: [], busy: false};

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

function setOnline(on) {
  C.online = on;
  $("#offline").hidden = on;
  if (on) flushQueue();
}

/* Messages wait here until the server takes them; a refusal (daemon down,
   bad input) is shown in the chat and the message is dropped, a network
   error keeps it for the next try. */
async function flushQueue() {
  if (C.busy) return;
  C.busy = true;
  while (C.queue.length) {
    try {
      await post("/api/chat", C.queue[0]);
      C.queue.shift();
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
  flushQueue();
}

function renderAll() {
  renderStrip(); renderAgents(); renderAttention(); renderTabs(); renderChat();
}
