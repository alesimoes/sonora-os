/* Quadro: tudo que está rodando (daemon e coordenador), PRs abertos e os freios,
   calculados em tools/dashboard_board.py, dashboard_prboard.py e dashboard_brakes.py. */
const LAUNCHER_PT={daemon:'daemon',coordenador:'coordenador'};
function agentRow(a,now){
  const pr=a.pr?`<a class="chip" href="${PRURL}${a.pr}" target="_blank" rel="noopener">PR #${a.pr}</a>`:'';
  const feat=a.feature?`<span class="chip">${E(a.feature)}</span>`:'';
  const dup=a.dup?`<div class="dupw">duplicado: ${E((a.dup_with||[]).join(', '))} no mesmo PR</div>`:'';
  const last=a.last?'há '+dur(Math.round(now-a.last)):'sem sinal';
  return `<tr class="${a.dup?'dup':''} ${a.kind==='run'?'clk':''}" ${a.kind==='run'?`data-k="run" data-id="${E(a.id)}"`:''}>
    <td><b>${E(a.role_pt)}</b><small>${E(a.name)}</small></td>
    <td>${E(a.what||'sem tarefa registrada')}<div>${feat}${pr}</div>${dup}</td>
    <td>${a.since?'há '+dur(Math.round(now-a.since)):'?'}</td>
    <td>${E(last)}${a.step?`<small title="${E(a.step)}">↳ ${E(a.step)}</small>`:''}<small>${E(a.state)}</small></td>
    <td><span class="lch ${a.launcher}">${E(LAUNCHER_PT[a.launcher]||a.launcher)}</span></td></tr>`}
function renderAgentsBoard(b,c,now){
  const rows=b.agents||[],dups=new Set(rows.filter(a=>a.dup).map(a=>a.pr));
  $('boardhint').textContent=rows.length+' rodando · '+rows.filter(a=>a.launcher==='daemon').length+' do daemon · '+rows.filter(a=>a.launcher==='coordenador').length+' do coordenador';
  $('boardwarn').hidden=!dups.size;
  $('boardwarn').textContent=dups.size?'Trabalho duplicado no PR '+[...dups].map(n=>'#'+n).join(', ')+': dois agentes no mesmo PR.':'';
  const coord=c&&c.updated&&now-c.updated<3600?`<div class="pnote">Coordenador: ${E(c.doing||'sem nota')} · atualizou há ${dur(Math.round(now-c.updated))}</div>`:'';
  $('boardagents').innerHTML=rows.length?`<table class="bt"><thead><tr><th>Quem</th><th>O quê</th><th>Desde</th><th>Último sinal</th><th>Lançado por</th></tr></thead><tbody>${rows.map(a=>agentRow(a,now)).join('')}</tbody></table>`+coord
    :'<div class="empty">Nenhum agente rodando agora.</div>'+coord}
const GATE_CLS={verde:'ok',rodando:'test',vermelho:'bad',conflito:'bad',base:'bad',cancelado:'warn',bloqueado:'warn',rascunho:'wait'};
function renderPrBoard(b){
  const rows=b.prs||[],late=rows.filter(p=>p.late);
  $('boardprhint').textContent=rows.length+' abertos · '+rows.filter(p=>p.gate==='verde').length+' verdes'+(late.length?' · '+late.length+' verde(s) sem merge':'');
  $('boardprs').innerHTML=rows.length?`<table class="bt"><thead><tr><th>PR</th><th>Gate</th><th>Dono do merge</th><th>Aberto há</th></tr></thead><tbody>${rows.map(p=>`<tr class="${p.late?'late':''} clk" data-k="pr" data-id="${p.number}">
    <td><a class="prn" href="${PRURL}${p.number}" target="_blank" rel="noopener">#${p.number}</a> ${E(shortTitle(p.title))}</td>
    <td><span class="gate ${GATE_CLS[p.gate]||''}">${E(p.gate_text)}</span>${p.late?`<div class="dupw">${E(p.late_text)}</div>`:''}</td>
    <td>${E(p.owner)}</td><td>${p.opened_min==null?'?':dur(p.opened_min*60)}</td></tr>`).join('')}</tbody></table>`:'<div class="empty">Nenhum PR aberto.</div>'}
function renderBrakes(b){
  const rows=b.brakes||[];
  $('boardbrakes').innerHTML=rows.map(r=>`<li class="${r.level}">${E(r.text)}</li>`).join('')||'<li class="ok">sem dados do daemon</li>'}
function renderBoard(s){
  const b=s.board||{},now=Date.now()/1000;
  renderBrakes(b);renderAgentsBoard(b,s.coordinator,now);renderPrBoard(b)}
