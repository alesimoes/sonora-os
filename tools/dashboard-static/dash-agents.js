const MODEL_PT=m=>m?String(m).replace(/^./,c=>c.toUpperCase()):'';
function personCard(x){
  const model=String(x.model||'').toLowerCase();
  const chips=(x.pr?`<a class="chip" href="${PRURL}${x.pr}" target="_blank" rel="noopener">PR #${x.pr}</a>`:'')+(x.branch?`<span class="chip">${E(x.branch)}</span>`:'');
  return `<article class="ag ${x.state} ${model==='opus'?'opus':''} clk" tabindex="0" role="button" data-k="${E(x.k)}" data-id="${E(x.id)}" title="clique para ver o detalhe">
    <div class="av">${pixelAvatar(x.avatar,model==='opus',48,x.seed||x.id)}</div>
    <div class="ab"><div class="an">${E(x.title)}</div>
      <div class="as">${model?`<span class="mdl ${model}">${E(MODEL_PT(model))}</span> `:''}${E(x.sub)}</div>
      <div class="sts"><i class="sd"></i>${E(x.stateText)}</div>
      <div class="ad">${E(x.doing||'sem tarefa registrada')}</div>${x.step?`<div class="ast" title="último passo do agente">↳ ${E(x.step)}</div>`:''}
      <div class="ac">${chips}<span class="more">detalhes ›</span></div></div></article>`}
function people(s){
  const now=Date.now()/1000,out=[];
  const c=s.coordinator;
  if(c&&c.updated&&now-c.updated<3600){const age=Math.round(now-c.updated),live=age<900;
    out.push({k:'coord',id:'coord',title:'Coordenador',sub:'Claude · lidera a sessão',model:'opus',avatar:'C',state:live?'working':'idle',
      stateText:live?'Trabalhando · atualizou há '+dur(age):'Sem atualização há '+dur(age),doing:(c.doing||'')+(c.next?' Próximo: '+c.next:'')})}
  s.agents.filter(a=>!['done','paused'].includes(a.state)).sort((x,y)=>ORDER[x.state]-ORDER[y.state]).forEach(a=>{
    const lk=a.lock,since=lk?dur(Math.round(now-lk.start)):'';
    const st=a.state==='building'?['working','Usando a máquina · há '+since]:a.state==='waiting_lock'?['wait','Esperando a máquina · há '+since]
      :a.state==='working'?['working','Trabalhando'+(a.since&&a.since_src!=='worktree'?' · há '+dur(Math.round(now-a.since)):'')]:['idle','Parado'];
    out.push({k:'agent',id:a.id,title:roleName(a.agent),sub:a.label,model:a.model,avatar:avatarOf(a.agent),state:st[0],stateText:st[1],doing:a.doing,pr:a.pr,branch:a.pr?'':a.branch})});
  const o=s.orchestrator;
  if(o){const age=o.updated_ts?Math.round(now-o.updated_ts):null,live=age!=null&&age<120,runs=(o.runs||[]).length;
    const doing=(runs?runs+' agente(s) rodando.':'Nenhum agente rodando.')+((o.paused||[]).length?' Filas pausadas: '+o.paused.join(', ')+'.':'');
    out.push({k:'builder',id:'orch',title:'Orquestrador',sub:'daemon · distribui o trabalho e faz o merge',model:'',avatar:'OR',state:live?'working':'idle',
      stateText:live?'Rodando · atualizou há '+dur(age):'Parado há '+dur(age),doing})}
  ((o&&o.runs)||[]).forEach(r=>{out.push({k:'run',id:r.id,title:roleName(r.role),sub:(r.agent||r.role)+' · '+(r.name||r.id),model:r.model||'sonnet',avatar:avatarOf(r.role),
    state:r.suspect||r.working===false?'wait':'working',stateText:(r.suspect?'Sem sinal':r.building?'Buildando':'Trabalhando')+(r.started?' · há '+dur(Math.round(now-r.started)):''),
    doing:r.title||r.task,step:r.last_step||(r.last_beat?'último sinal há '+dur(Math.round(now-r.last_beat)):''),pr:r.pr,branch:r.branch})});
  return out}
function renderPeople(s){
  const list=people(s);
  $('people').innerHTML=list.map(personCard).join('')||'<div class="empty">Nenhum agente.</div>';
  const o=s.orchestrator,runs=((o&&o.runs)||[]).length;
  $('pnote').textContent=o?(runs?'':'Orquestrador automático: nenhum agente em execução'+((o.paused||[]).length?' (as filas de planejamento e construção estão pausadas)':'')+'.'):'Orquestrador automático desligado.'}
