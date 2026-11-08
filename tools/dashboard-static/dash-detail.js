/* detalhes do card clicado: agente (o que está fazendo), feature/tarefa, PR */
let detOpen=null,detTimer=null;
const ago=t=>t?dur(Math.max(0,Math.round(Date.now()/1000-t)))+' atrás':'-';
function kv(o){return '<div class="kv">'+Object.entries(o).filter(([,v])=>v!=null&&v!==''&&!(Array.isArray(v)&&!v.length)).map(([k,v])=>`<b>${E(k)}</b><span>${E(Array.isArray(v)?v.join(', '):typeof v==='object'?JSON.stringify(v):v)}</span>`).join('')+'</div>'}
function taskHtml(t){if(!t)return '';
  return `<h4>Tarefa ${E(t.id)}</h4>`+kv({estado:t.state,agente:t.agent,trilha:t.track,branch:t.branch,PR:t.pr,tentativas:t.attempts,correção:t.fix_kind,arquivos:t.nfiles?t.nfiles+' ('+t.files.join(', ')+(t.nfiles>t.files.length?', …':'')+')':''})
   +(t.brief?`<h4>Resumo</h4><pre>${E(t.brief)}</pre>`:'')+(t.note?`<h4>Nota / motivo atual</h4><pre>${E(t.note)}</pre>`:'')}
function evHtml(ev){return ev&&ev.length?'<h4>Histórico</h4>'+ev.slice().reverse().map(e=>`<div class="ev">${E(ago(e.ts))} · <b>${E(e.kind)}</b> ${E(e.detail||'')}</div>`).join(''):''}
function detHtml(d){
  if(d.error)return `<p class="bad">${E(d.error)}</p>`;
  if(d.kind==='coord'){const c=d.coord||{};return kv({agora:c.doing,atualizado:ago(c.updated)})+(c.steps&&c.steps.length?'<h4>Passos (mais recente primeiro)</h4>'+c.steps.slice().reverse().map(x=>`<div class="act ${x.done?'fim':'texto'}"><i>${x.done?'feito':'em curso'}</i>${E(x.text)}</div>`).join(''):'')+(c.waits?`<h4>Espera você</h4><pre>${E(c.waits)}</pre>`:'')}
  if(d.kind==='agent'){const a=d.agent||{};
    return kv({estado:NAME[a.state]||a.state,modelo:a.model,agente:a.agent,trabalho:a.doing,PR:a.pr,branch:a.branch,'desde':a.since?clk(a.since)+' ('+ago(a.since)+')':'',trilha:a.track,'arquivos modificados':a.dirty,containers:a.containers,id:a.id})
      +(a.lock?'<h4>Fila da máquina</h4>'+kv({papel:a.lock.role==='holds'?'usando':'esperando',prioridade:a.lock.prio,'desde':ago(a.lock.start),comando:a.lock.cmd}):'')
      +'<h4>Quem usa a máquina</h4>'+lockRows(d.lock)
      +(d.not_reachable?`<h4>Conversa</h4><div class="ev">${E(d.not_reachable)}</div>`:'')}
  if(d.kind==='run'){const r=d.run||{};
    const act=(d.activity||[]).slice().reverse().map(a=>`<div class="act ${E(a.kind)}"><i>${E(a.kind)}</i>${E(a.text)}</div>`).join('')||'<div class="ev">sem atividade registrada ainda</div>';
    return (r.ended?'':'<div class="dact">'+runActs(r.name)+'</div>')+kv({papel:r.role,estado:r.state,começou:ago(r.started),'último sinal':ago(r.last_beat),fim:r.ended?ago(r.ended)+' (exit '+r.exit+')':''})
      +'<h4>O que está fazendo (mais recente primeiro)</h4>'+act+taskHtml(d.task)+evHtml(d.events)+chatHtml(d.chat)}
  const f=d.feature||{},tr=d.train;
  return kv({estado:f.state,trilha:f.track,PR:f.pr,branch:f.branch,correções:f.fixes,bloqueio:f.blocked_reason})
    +(tr?'<h4>No merge train</h4>'+kv({fase:tr.phase,próximo:tr.next,'última ação':tr.last_action,checks:(tr.passed??'?')+'/'+(tr.total??'?'),falhando:tr.failing,fixer:tr.fixer}):'')
    +(d.runs&&d.runs.length?'<h4>Agentes</h4>'+d.runs.slice().reverse().map(r=>`<div class="act clk" data-k="run" data-id="${E(r.id)}"><i>${E(r.role)}</i>${E(r.name||r.id)} · ${r.ended?'terminou '+ago(r.ended):'rodando desde '+ago(r.started)}</div>`).join(''):'')
    +(d.tasks||[]).map(taskHtml).join('')+evHtml(d.events)}
async function loadDetail(){if(!detOpen)return;
  try{const r=await fetch(`/api/detail?kind=${encodeURIComponent(detOpen.k)}&id=${encodeURIComponent(detOpen.id)}`,{cache:'no-store'});const d=await r.json();
    const box=$('chatbox'),keep=box?box.value:'',had=box&&document.activeElement===box,st=$('chatst')?$('chatst').textContent:'';
    lastChatName=d.chat?d.chat.name:'';
    $('dtitle').textContent=d.title||detOpen.id;$('dbody').innerHTML=detHtml(d);
    const nb=$('chatbox');if(nb){nb.value=keep;if(had)nb.focus();if(st&&box)$('chatst').textContent=st}}catch(e){$('dbody').innerHTML=`<p class="bad">${E(String(e))}</p>`}}
function openDetail(k,id){detOpen={k,id};$('dtitle').textContent=id;$('dbody').innerHTML='<div class="ev">carregando…</div>';$('drawer').classList.add('on');
  clearInterval(detTimer);loadDetail();detTimer=setInterval(loadDetail,4000)}
function closeDetail(){detOpen=null;clearInterval(detTimer);$('drawer').classList.remove('on')}
document.addEventListener('click',e=>{if(e.target.closest('a,button,input,label,.kh,h3[onclick]'))return;const el=e.target.closest('[data-k]');if(el)openDetail(el.dataset.k,el.dataset.id)});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeDetail();if(e.key==='Enter'){const el=e.target.closest&&e.target.closest('[data-k]');if(el&&el.tagName!=='A')openDetail(el.dataset.k,el.dataset.id)}});
