/* ações do painel: botões por item e o painel de comandos. O servidor só aceita a lista
   fechada de dashboard_actions*.py; cada POST leva o token desta página, e a ação destrutiva
   pede confirmação aqui e de novo no servidor (confirm:true). A saída aparece no aviso. */
const DASH_TOKEN=(document.querySelector('meta[name=dash-token]')||{}).content||'';
let ACTIONS={},actJobs=[],actLog=[];
const ACT_GLOBAL=[['daemon.status','status do daemon',{}],['daemon.restart','reiniciar daemon',{}],['daemon.update','atualizar para a main e reiniciar',{}],
  ['supervisor.start','iniciar supervisor',{}],['supervisor.stop','parar supervisor',{}],['disk.preview','ver o que a limpeza libera',{}]];

function actBtn(label,action,params,cls){
  return `<button type="button" class="abtn ${cls||''}" data-act="${E(action)}" data-params="${E(JSON.stringify(params||{}))}" title="${E(action)}">${E(label)}</button>`}
function actToast(html){const t=$('acttoast');t.innerHTML=html+'<button type="button" class="abtn" onclick="this.parentNode.hidden=true">fechar</button>';t.hidden=false}
async function actPost(action,params,confirm){
  const r=await fetch('/api/action',{method:'POST',headers:{'Content-Type':'application/json','X-Dash-Token':DASH_TOKEN},
    body:JSON.stringify({action,params,confirm})});
  return [r.status,await r.json()]}
async function actRun(action,params){
  const meta=ACTIONS[action]||{},label=meta.label||action,what=label+' '+JSON.stringify(params);
  if(meta.destructive&&!confirm('Confirmar: '+what+'?'))return;
  actToast('<b>'+E(label)+'</b> enviando…');
  const [code,j]=await actPost(action,params,!!meta.destructive);
  if(code!==202){actToast('<b>'+E(label)+'</b> recusada: '+E(j.error||code));return}
  for(let i=0;i<900;i++){
    await new Promise(r=>setTimeout(r,1000));
    const r=await fetch('/api/action?id='+j.job,{cache:'no-store'}),v=await r.json();
    actToast('<b>'+E(label)+'</b> '+E(v.state)+(v.output?`<pre>${E(v.output)}</pre>`:''));
    if(v.state!=='running'){loadActions();return}}}
document.addEventListener('click',e=>{const b=e.target.closest('button[data-act]');if(!b)return;
  e.stopPropagation();actRun(b.dataset.act,JSON.parse(b.dataset.params||'{}'))});

async function loadActions(){
  try{const r=await fetch('/api/actions',{cache:'no-store'}),j=await r.json();
    ACTIONS=Object.fromEntries(j.actions.map(a=>[a.name,a]));actJobs=j.jobs;actLog=j.log;renderCmds()}catch(e){}}
function renderCmds(){
  const when=t=>new Date(t*1000).toLocaleTimeString('pt-BR');
  $('cmds').innerHTML=ACT_GLOBAL.filter(a=>ACTIONS[a[0]]).map(a=>actBtn(a[1],a[0],a[2],ACTIONS[a[0]].destructive?'dng':'')).join('')
    +'<div class="alog"><b>Ações recentes</b>'+(actLog.length?actLog.map(x=>`<div class="trow ${x.ok?'':'bad'}"><small>${when(x.ts)}</small><span class="tt">${E(x.action)} ${E(JSON.stringify(x.params))}</span><small>${E(x.result)}</small></div>`).join(''):'<div class="empty">nenhuma ainda</div>')+'</div>'}

/* botões por item, usados pelos outros painéis */
const prActs=(n,phase)=>actBtn('adotar','pr.adopt',{pr:n})+(phase==='verde'?actBtn('merge agora','pr.merge',{pr:n},'dng'):'');
const runActs=name=>actBtn('parar','run.stop',{name},'dng')+actBtn('relançar','run.relaunch',{name});
const poolActs=(n,paused)=>actBtn(paused?'retomar':'pausar',paused?'pool.resume':'pool.pause',{pool:n});
function alertsHtml(o){const a=(o&&o.alerts)||[];
  return a.length?a.map(x=>`<div class="alert">${E(typeof x==='object'?(x.text||x.message||x.reason||JSON.stringify(x)):x)}</div>`).join(''):''}
function derivedAlerts(s){const o=s.orchestrator,out=[],now=Date.now()/1000;
  const st=s.prs.map(p=>[p,prState(p)]),nm=c=>st.filter(x=>x[1].col===c&&x[1].cls!=='wait').map(x=>'#'+x[0].number).join(' ');
  if(!o)out.push('Daemon do orquestrador desligado: nada será mergeado nem distribuído.');
  else if(!o.updated_ts||now-o.updated_ts>120)out.push('Daemon sem sinal há '+dur(Math.round(now-(o.updated_ts||0)))+'.');
  const ready=nm(4);if(ready&&o&&((o.paused||[]).length||!o.updated_ts||now-o.updated_ts>120))out.push('PRs prontos esperando merge com o orquestrador pausado: '+ready);
  const bad=nm(3);if(bad)out.push('PRs que precisam de correção: '+bad);
  s.prs.filter(p=>p.base&&p.base!=='main').forEach(p=>out.push('PR #'+p.number+' empilhado na base '+p.base+' (proibido)'));
  return out}
function renderAlerts(s){const h=alertsHtml(s.orchestrator)+derivedAlerts(s).map(x=>`<div class="alert">${E(x)}</div>`).join('');$('alerts').hidden=!h;$('alerts').innerHTML=h}
loadActions();setInterval(loadActions,15000);
