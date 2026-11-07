/* Estado de cada PR a partir do que o GitHub diz (checks e estado de merge). O gate local
   (lotes do builder) está sendo aposentado e não decide mais a coluna. col: 2 checks,
   3 corrigindo, 4 pronto. PR com outra base é proibido: base diferente de main vira alerta. */
function prState(p){
  const failed=(p.failed||[]).length-(p.cancelled||[]).length,pend=(p.pending||[]).length;
  if(p.draft)return {col:1,cls:'wait',text:'rascunho (agente ainda escrevendo)',hold:true};
  if(p.base&&p.base!=='main')return {col:3,cls:'bad',text:'base '+p.base+': PR com base fora da main não é permitido',hold:true};
  if(p.merge==='DIRTY'||p.mergeable==='CONFLICTING')return {col:3,cls:'bad',text:'conflito com a main'};
  if(failed>0)return {col:3,cls:'bad',text:'falhou: '+p.failed.filter(n=>!(p.cancelled||[]).includes(n)).slice(0,2).join(', ')};
  if(pend)return {col:2,cls:'test',text:'checks rodando · '+p.passed+' de '+p.total};
  if(!p.total)return {col:2,cls:'wait',text:'sem checks ainda'};
  if((p.cancelled||[]).length)return {col:2,cls:'wait',text:'checks cancelados: rodar de novo',hold:true};
  if(p.merge==='BEHIND')return {col:4,cls:'ok',text:'pronto, atrás da main (atualizar)'};
  if(p.merge==='BLOCKED')return {col:2,cls:'wait',text:'bloqueado: falta check obrigatório ou revisão',hold:true};
  return {col:4,cls:'ok',text:p.merge==='UNSTABLE'?'pronto (check opcional falhou ou pendente)':'pronto para o merge'}}
function renderSit(s){
  const o=s.orchestrator||{},rows=s.prs.map(p=>({p,st:prState(p)}));
  const by=c=>rows.filter(r=>r.st.col===c),nums=a=>a.map(r=>'#'+r.p.number).join(' ');
  const bad=by(3),ready=by(4),test=by(2).concat(rows.filter(r=>r.st.col===1));
  const owner=(o.waits_on_owner||[]).length;
  const pg=s.progress,pgBar=p=>`<span class="pgbar"><i style="width:${p}%"></i></span>`;
  const prog=pg?`<span class="pgm" title="Só conta o que está na main e verificado (planning/progress.json): ${pg.done}/${pg.total} pontos, +${pg.doing} em andamento">Projeto <b>${pg.pct}%</b> ${pgBar(pg.pct)}${(pg.fronts||[]).map(f=>` · ${E(f.title)} <b>${f.pct}%</b> ${pgBar(f.pct)}`).join('')}</span>`:'<span>Projeto: sem dado de progresso</span>';
  const flow=`<span class="sflow" title="PRs abertos: checks, correção, pronto, entrou hoje"><b>${s.prs.length}</b> PRs abertos → <b>${test.length}</b> testando · <b class="${bad.length?'sbad':''}">${bad.length}</b> corrigindo · <b>${ready.length}</b> prontos → <b>${(s.merged||[]).length}</b> na main hoje</span>`;
  const act=bad.length?'corrigir '+nums(bad):ready.length?'merge de '+nums(ready):test.length?'esperar checks de '+nums(test):'nada pendente';
  $('sit').innerHTML=prog+flow+`<span>Próximo: <b>${E(act)}</b></span>`+(owner?`<span class="sitw">${owner} decisão(ões) do dono</span>`:'')}
