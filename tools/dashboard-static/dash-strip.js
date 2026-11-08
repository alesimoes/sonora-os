/* faixa de status pequena: disco, daemon, máquina (lock). Contexto e consumo são os medidores
   do topo. Só mostra cor de alerta quando algo exige ação. */
function chip(cls,label,val,title,extra){return `<span class="sc ${cls||''}" title="${E(title||'')}" ${extra||''}><small>${E(label)}</small> <b>${E(val)}</b></span>`}
let pauseOpen=false;
document.addEventListener('click',e=>{if(e.target.closest('[data-pausetoggle]')){pauseOpen=!pauseOpen;const b=$('pausebox');if(b)b.hidden=!pauseOpen||!b.innerHTML}});
const BUILD_POOLS=['build','build:daw','build:os','build:plugin'];
function renderPauseBox(o){const paused=(o&&o.paused)||[],b=$('pausebox'),nb=BUILD_POOLS.filter(n=>paused.includes(n)).length;
  const bulk=(nb?actBtn('Retomar toda a construção ('+nb+')','pool.resume_group',{group:'build'}):'')+(paused.length?actBtn('Retomar tudo ('+paused.length+')','pool.resume_group',{group:'all'}):'');
  b.innerHTML=paused.length?'<b>Filas pausadas</b><span class="pbulk">'+bulk+'</span>'+paused.map(n=>`<span class="pz"><span>${E((POOL_PT[n]||[n])[0])}<small> · ${E(POOL_HINT[n]||'')}</small></span>${poolActs(n,true)}</span>`).join(''):'';
  b.hidden=!pauseOpen||!paused.length}
function renderStrip(s){
  const o=s.orchestrator,now=Date.now()/1000,d=s.disk&&s.disk.drive,lock=s.lock||[],holder=lock.find(r=>r.role==='holds'),nw=lock.filter(r=>r.role==='waits').length;
  const c=[];
  if(!o)renderPauseBox(null);
  if(!o)c.push(chip('bad','Daemon','desligado','sem status.json do orquestrador'));
  else{const age=o.updated_ts?Math.round(now-o.updated_ts):null,stale=age==null||age>120,paused=o.paused||[],runs=(o.runs||[]).length;
    c.push(chip(stale?'bad':paused.length?'warn':'ok','Daemon',stale?'parado há '+dur(age):'vivo',(o.mode||'')+' · atualizou há '+dur(age)));
    c.push(chip(paused.length?'warn':'','Agentes',runs+' de '+(o.cap||'?')+(paused.length?' · '+paused.length+' filas pausadas':''),paused.length?'clique para ver quais e retomar':'',paused.length?'data-pausetoggle role="button" tabindex="0"':''));
    renderPauseBox(o);
    const h=o.budget_holds||{},hold=h.work||h.planner;
    c.push(chip(h.work?'warn':'','Execuções/h',(o.runs_last_hour??'?')+' de '+(o.max_runs_per_hour||'?')+' · planners '+(o.planner_runs_last_hour??'?')+' de '+(o.max_planner_runs_per_hour||'?'),hold?'retido: '+(h.work?'trabalho ':'')+(h.planner?'planners ':'')+'(limite por hora)':'lançamentos na última hora; planners têm orçamento próprio'));
    c.push(chip('','PRs abertos',(o.open_prs??s.prs.length)+' de '+(o.max_open_prs||'?'),'limite do orquestrador'))}
  c.push(chip(holder?'':'','Build pesado',holder?String(holder.who||'').slice(0,28)+(nw?' +'+nw+' na fila':''):'livre','trava da máquina (build-slot.sh)'));
  if(d&&d.ok)c.push(chip(d.level==='ok'?'':d.level,'Disco',bytes(d.free)+' livres',d.level==='crit'?'CRÍTICO: menos de 10 GB':d.level==='warn'?'Atenção: menos de 30 GB':'espaço suficiente'));
  const n=s.orchestrator&&s.orchestrator.nightly;
  if(o){const l=n&&n.last,age=l&&l.at?now-l.at:null,old=age==null||age>(n&&n.watch_s||93600),bad=!l||l.conclusion!=='success'||old,
    val=l?(l.conclusion||'?')+' · '+(age==null?'?':'há '+dur(age))+' · '+l.sha.slice(0,8):'sem execução';
    c.push(chip(bad?'bad':'ok','Nightly',val,'última execução na main'+(n&&n.dispatched_at?' · disparada pelo daemon há '+dur(now-n.dispatched_at):'')+(old?' · mais velha que 26 h':''),l&&l.url?`onclick="window.open('${E(l.url)}')" role="link" tabindex="0"`:''))}
  $('strip').innerHTML=c.join('')}
