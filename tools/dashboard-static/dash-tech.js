const POOL_PT={'plan':['Planejamento','AR'],'build':['Construção (geral)','IN'],'build:daw':['Construção · DAW','UI'],'build:os':['Construção · OS','OS'],'build:plugin':['Construção · plugins','PL'],'qa':['Testes (QA)','QA'],'fix':['Correção','FX'],'merge':['Merge','IN'],'principal':['Engenheiro principal','P']};
const POOL_HINT={'plan':'planejamento das tarefas','build':'tarefas sem trilha definida','build:daw':'tarefas da trilha DAW (motor e interface)','build:os':'tarefas da trilha OS (imagem e instalador)','build:plugin':'tarefas da trilha de plugins nativos','qa':'testes de aceitação','fix':'correções de PRs vermelhos','merge':'integração de conflitos','principal':'escalonamentos difíceis'};
function renderTech(s){
  const o=s.orchestrator,now=Date.now()/1000;
  if(!o){$('tlimits').innerHTML='<div class="empty">Orquestrador desligado.</div>';$('tpools').innerHTML='';$('tmeta').textContent=''}
  else{
    const age=o.updated_ts?Math.round(now-o.updated_ts):null,paused=o.paused||[];
    $('tmeta').textContent=(String(o.mode||'')==='live'?'ligado':'modo '+o.mode)+(age!=null?' · atualizou há '+dur(age):'');
    const runs=(o.runs||[]).length,cap=Number(o.cap)||0;
    const st=(n,l)=>`<div class="stat"><b>${E(n)}</b><span>${E(l)}</span></div>`;
    let h=(age>120?`<div class="banner bad">Orquestrador parado desde ${clk(o.updated_ts)}: o painel mostra o último estado conhecido.</div>`:'')
      +(paused.length?`<div class="banner">Filas pausadas: ${E(paused.join(', '))}</div>`:'')
      +(o.kill_switch?`<div class="banner bad">Chave de emergência ligada: ${E(typeof o.kill_switch==='object'?JSON.stringify(o.kill_switch):o.kill_switch)}</div>`:'')
      +(o.usage_pause_until?`<div class="banner">Pausa por consumo até ${clk(Number(o.usage_pause_until)||Date.parse(o.usage_pause_until)/1000)}</div>`:'')
      +'<div class="stats">'+st(runs+' de '+(cap||'?'),'agentes rodando / limite')+st((o.open_prs??0)+' de '+(o.max_open_prs||'?'),'PRs abertos / limite')
      +st(o.ready_unpr??0,'prontos esperando PR')+st(o.runs_last_hour??0,'execuções na última hora')+st((o.reservations||[]).length,'reservas de ADR e protocolo')+'</div>';
    $('tlimits').innerHTML=h;
    const P=o.pools||{};
    $('tpools').innerHTML=Object.keys(P).filter(n=>(P[n].floor||0)>0||(P[n].live||0)>0||paused.includes(n)).map(n=>{const p=P[n],pp=POOL_PT[n]||[n,'AG'];
      const isP=p.paused||p.reason==='paused'||paused.includes(n),fl=p.floor||0,lv=p.live||0;
      return `<div class="pool ${isP?'paused':lv<fl?'low':''}"><span class="pa">${pixelAvatar(pp[1],false,24,'pool-'+n)}</span><div><b>${E(pp[0])}</b><span>${isP?'pausado':lv+' ativos'+(fl?' · mínimo '+fl:'')}</span>${poolActs(n,isP)}</div></div>`}).join('')}
  const w=(o&&o.waits_on_owner)||[];
  $('townerp').hidden=!w.length;
  $('towner').innerHTML=w.length?`<div class="owner"><ul>${w.map(x=>`<li>${E(typeof x==='object'?(x.title||x.text||x.question||JSON.stringify(x)):x)}</li>`).join('')}</ul></div>`:'';
  const bg=((o&&o.blocked)||[]).filter(g=>g&&g.items);
  $('tblockp').hidden=!bg.length;
  $('tblock').innerHTML=bg.map(g=>`<div class="owner"><b>${E(g.label||g.cause)} (${g.items.length})</b><ul>${g.items.map(x=>`<li>${E(x.task)}: ${E(String(x.note||'').slice(0,160))}</li>`).join('')}</ul></div>`).join('');
  $('wcount').textContent=s.watchers.length+' ativos';
  $('ws').innerHTML=s.watchers.map(x=>`<div class="trow"><a class="prn" href="${PRURL}${x.pr}" target="_blank" rel="noopener">PR #${x.pr}</a><span class="tt">espera ficar verde e faz o merge</span><small>pid ${E(x.pid)}</small></div>`).join('')||'<div class="empty">Nenhum merge automático armado.</div>';
  $('ccount').textContent=s.containers.length+' rodando';
  const own=c=>s.agents.find(a=>a.containers.includes(c.id));
  $('cs').innerHTML=s.containers.map(c=>{const a=own(c);
    return `<div class="trow"><span class="pa">${pixelAvatar(a?avatarOf(a.agent):'BD',false,24,a?a.id:'c'+c.id)}</span><span class="tt">${a?E(a.label):'sem dono conhecido'}<br><small>${E(c.image)}</small></span><small>${dur(Math.round(now-Date.parse(c.started)/1000))}</small></div>`}).join('')||'<div class="empty">Nenhum container.</div>'}

const techEl=$('tech');if(localStorage.getItem('sonora.tech'))techEl.open=true;techEl.addEventListener('toggle',()=>localStorage.setItem('sonora.tech',techEl.open?'1':''));
