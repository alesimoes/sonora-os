function prStatus(p,t){
  if(t){const ph=t.phase||'';
    if(ph==='conflito')return ['bad','conflito com a main'];
    if(t.state==='ci_fix'||ph==='fixer')return ['bad','falhou: '+((t.failing||[]).slice(0,2).join(', ')||'testes')];
    if(ph==='empilhado'||ph==='retarget')return ['wait','esperando outro PR'+(t.waits_on?' (#'+t.waits_on+')':'')];
    if(ph==='verde')return ['ok','pronto'];
    if(ph==='juntando')return ['wait','juntando PRs'];
    if(ph==='bloqueado'||ph==='fechado')return ['bad','bloqueado'];
    return ['test','testando']}
  if(p.failed.length>(p.cancelled||[]).length)return ['bad','falhou: '+p.failed.slice(0,2).join(', ')];
  if(p.base&&p.base!=='main')return ['wait','esperando outro PR'];
  if(p.pending.length)return ['test','testando'];
  return ['ok','pronto']}
function renderPrs(s){
  const tr=Object.fromEntries(((s.orchestrator||{}).train||[]).map(r=>[r.pr,r]));
  return s.prs.map(p=>({p,t:tr[p.number],st:prStatus(p,tr[p.number])}))}
function renderSit(s){
  const lock=s.lock||[],holder=lock.find(r=>r.role==='holds'),nw=lock.filter(r=>r.role==='waits').length;
  const o=s.orchestrator||{},rows=renderPrs(s);
  const bad=rows.filter(r=>r.st[0]==='bad'),ready=rows.filter(r=>r.st[1]==='pronto'),test=rows.filter(r=>r.st[0]==='test'),stk=rows.filter(r=>r.st[1].startsWith('esperando'));
  const nums=a=>a.map(r=>'#'+r.p.number).join(' ');
  const next=bad.length?'corrigir '+nums(bad)+' (falhou)':ready.length?'fazer o merge de '+nums(ready):test.length?'terminar o teste de '+nums(test)+(stk.length?', depois '+nums(stk):''):stk.length?'esperar '+nums(stk):'nada pendente';
  const owner=(o.waits_on_owner||[]).length;
  const age=o.updated_ts?Date.now()/1000-o.updated_ts:0;
  const coordNext=(s.coordinator&&s.coordinator.next)||'';
  const run=holder?E(String(holder.who||'').replace(/\s*\(.*\)$/,'').slice(0,40)):'nada';
  const pg=s.progress,pgBar=p=>`<span class="pgbar"><i style="width:${p}%"></i></span>`;
  const prog=pg?`<span title="Só conta o que está na main e verificado (planning/progress.json): ${pg.done}/${pg.total} pontos, +${pg.doing} em andamento">Projeto: <b>${pg.pct}%</b> ${pgBar(pg.pct)}${(pg.fronts||[]).map(f=>` · ${E(f.title)} <b>${f.pct}%</b>`).join('')}</span>`:'';
  $('sit').innerHTML=prog+`<span><b>${(s.merged||[]).length}</b> merges hoje</span><span>Máquina: <b>${run}</b>${nw?' · '+nw+' na fila':''}</span><span>Próximo: <b>${E(coordNext||next)}</b></span>`
    +diskSit(s.disk)+(owner?`<span class="sitw">${owner} decisão(ões) do dono</span>`:'')+(age>120?'<span class="sitb">orquestrador pausado</span>':'')}

function diskSit(d){const dr=d&&d.drive;
  return dr&&dr.ok&&dr.level!=='ok'?`<span class="${dr.level==='crit'?'sitc':'sitw'}">Disco: só ${bytes(dr.free)} livres</span>`:''}
