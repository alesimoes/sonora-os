/* faixa de status pequena: disco, daemon, máquina (lock). Contexto e consumo são os medidores
   do topo. Só mostra cor de alerta quando algo exige ação. */
function chip(cls,label,val,title){return `<span class="sc ${cls||''}" title="${E(title||'')}"><small>${E(label)}</small> <b>${E(val)}</b></span>`}
function renderStrip(s){
  const o=s.orchestrator,now=Date.now()/1000,d=s.disk&&s.disk.drive,lock=s.lock||[],holder=lock.find(r=>r.role==='holds'),nw=lock.filter(r=>r.role==='waits').length;
  const c=[];
  if(!o)c.push(chip('bad','Daemon','desligado','sem status.json do orquestrador'));
  else{const age=o.updated_ts?Math.round(now-o.updated_ts):null,stale=age==null||age>120,paused=o.paused||[],runs=(o.runs||[]).length;
    c.push(chip(stale?'bad':paused.length?'warn':'ok','Daemon',stale?'parado há '+dur(age):'vivo',(o.mode||'')+' · atualizou há '+dur(age)));
    c.push(chip(paused.length?'warn':'','Agentes',runs+' de '+(o.cap||'?')+(paused.length?' · '+paused.length+' filas pausadas':''),paused.join(', ')));
    c.push(chip('','PRs abertos',(o.open_prs??s.prs.length)+' de '+(o.max_open_prs||'?'),'limite do orquestrador'))}
  c.push(chip(holder?'':'','Build pesado',holder?String(holder.who||'').slice(0,28)+(nw?' +'+nw+' na fila':''):'livre','trava da máquina (build-slot.sh)'));
  if(d&&d.ok)c.push(chip(d.level==='ok'?'':d.level,'Disco',bytes(d.free)+' livres',d.level==='crit'?'CRÍTICO: menos de 10 GB':d.level==='warn'?'Atenção: menos de 30 GB':'espaço suficiente'));
  $('strip').innerHTML=c.join('')}
