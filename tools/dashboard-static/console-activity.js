/* The selected agent's last steps, above the chat (owner, 2026-11-09: tapping
   an agent showed nothing of what it was doing). Polled while one is picked;
   redrawn only when the steps change, so it does not blink. */
"use strict";
const ACT_POLL_MS = 3000;
const TOOL_PT = {Bash: "comando", Read: "lendo", Edit: "editando", Write: "escrevendo",
  Grep: "buscando", Glob: "listando", send: "mensagem", recv: "esperando"};
let actTimer = null, actFor = null;

function stepHtml(s) {
  const label = s.kind === "text" ? "diz" : (TOOL_PT[s.tool] || s.tool);
  return `<li class="${s.kind}"><time>${hhmm(s.ts)}</time><b>${esc(label)}</b><span>${esc(s.text)}</span></li>`;
}

function renderActivity(a) {
  const box = $("#act");
  if (!a) { box.hidden = true; setHTML(box, ""); return; }
  box.hidden = false;
  const steps = a.steps.length ? a.steps.slice().reverse().map(stepHtml).join("")
    : '<li class="text"><span>Ainda sem passos registrados.</span></li>';
  const atTop = !box.querySelector("ol") || box.querySelector("ol").scrollTop < 20;
  if (!setHTML(box, `<div class="ah"><span>${esc(a.name)}</span><span>${esc(a.model)}</span>` +
      `<button type="button" id="actx" aria-label="Fechar">fechar</button></div>` +
      `<p class="atitle">${esc(a.title || a.task)}</p><ol>${steps}</ol>`)) return;
  if (atTop) box.querySelector("ol").scrollTop = 0;
  $("#actx").onclick = () => setTarget(null);
}

async function pollActivity() {
  const name = actFor;
  if (!name) return;
  try {
    const r = await fetch(`/api/agent/${encodeURIComponent(name)}/activity`, {cache: "no-store"});
    const j = await r.json();
    if (actFor !== name) return;
    renderActivity(r.ok ? j : {name, model: "", task: j.error || "", steps: []});
  } catch (e) { /* the next tick tries again */ }
}

/* Called by setTarget: starts or stops following that agent. */
function followAgent(name) {
  actFor = name || null;
  clearInterval(actTimer);
  actTimer = null;
  if (!actFor) { renderActivity(null); return; }
  renderActivity({name: actFor, model: "", task: "carregando…", steps: []});
  pollActivity();
  actTimer = setInterval(pollActivity, ACT_POLL_MS);
}
