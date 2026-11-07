/* máquina/disco compacto: uma linha de resumo sempre visível e, aberto, uma tabela do Docker e os 3 maiores
   consumidores. Alerta só com disco abaixo de 15% livre ou mais de 30 GB recuperáveis no Docker. */
const GIB=1024**3;
const bytes=n=>n==null||n<0?'?':n>=1e12?(n/1e12).toFixed(2)+' TB':n>=1e9?(n/1e9).toFixed(1)+' GB':n>=1e6?Math.round(n/1e6)+' MB':Math.round(n/1e3)+' kB';
const DOCKER_PT={'Images':'Imagens','Containers':'Containers','Local Volumes':'Volumes','Build Cache':'Cache de build'};

const LOW_FREE_PCT=15,HIGH_RECLAIM=30e9,TOP_N=3;
const freePct=d=>d&&d.ok&&d.total?Math.round(d.free/d.total*100):null;
const reclaimTotal=k=>k&&k.ok?(k.rows||[]).reduce((a,r)=>a+(r.reclaimable||0),0):null;
function diskWarn(d,k){const p=freePct(d),r=reclaimTotal(k);return (p!=null&&p<LOW_FREE_PCT)||(r!=null&&r>HIGH_RECLAIM)||(d&&d.ok&&d.level!=='ok')}

function sysSummary(s){
  const d=(s.disk||{}).drive,k=(s.disk||{}).docker,r=(s.disk||{}).repo,lock=s.lock||[],holder=lock.find(x=>x.role==='holds');
  const p=[`Build: ${holder?'ocupado'+(lock.length>1?' (+'+(lock.length-1)+' na fila)':''):'livre'}`];
  if(d&&d.ok)p.push(`Disco ${bytes(d.free)} livres (${freePct(d)}%)`);
  if(k&&k.ok)p.push(`Docker ${bytes(reclaimTotal(k))} recuperáveis`);
  if(r&&r.ok&&r.worktrees)p.push(`worktrees ${bytes(r.worktrees.size)}`);
  return p.join(' · ')}

function dockerTable(k){
  if(!k||!k.ok)return `<div class="empty">Docker: ${E((k&&k.error)||'sem dado')} (o painel segue sem ele)</div>`;
  const rows=(k.rows||[]).filter(r=>r.count>0||r.size>0).map(r=>`<tr><td>${E(DOCKER_PT[r.type]||r.type)}</td><td>${r.active}/${r.count}</td><td>${bytes(r.size)}</td><td>${r.reclaimable>0?bytes(r.reclaimable):'-'}</td></tr>`).join('');
  return `<table class="dtab"><tr><th>Docker</th><th>em uso</th><th>tamanho</th><th>recuperável</th></tr>${rows}</table>`}

function topConsumers(r){
  if(!r||!r.ok)return `<div class="empty">Consumidores: ${E((r&&r.error)||'sem dado')}</div>`;
  const all=[...((r.worktrees||{}).top||[]).map(x=>({n:'worktrees/'+x.name,size:x.size})),...((r.builds||{}).top||[]).map(x=>({n:x.name,size:x.size}))]
    .filter(x=>x.size>0).sort((a,b)=>b.size-a.size).slice(0,TOP_N);
  if(!all.length)return '';
  return `<table class="dtab"><tr><th>Maiores consumidores${r.partial?' (parcial)':''}</th><th>tamanho</th></tr>${all.map(x=>`<tr><td class="mono">${E(x.n)}</td><td>${bytes(x.size)}</td></tr>`).join('')}</table>`}

function renderDisk(s){
  const d=s.disk||{},warn=diskWarn(d.drive,d.docker),crit=d.drive&&d.drive.level==='crit';
  const lowMsg=warn?`<div class="dwarn">${crit?'CRÍTICO: menos de 10 GB livres; o disco cheio já derrubou o daemon. ':''}Disco ${bytes((d.drive||{}).free)} livres, Docker ${bytes(reclaimTotal(d.docker))} recuperáveis.</div>`:'';
  $('disk').innerHTML=lowMsg+dockerTable(d.docker)+topConsumers(d.repo)
    +`<div class="dact">${actBtn('ver o que libera','disk.preview',{})}${actBtn('limpar','disk.clean',{},warn?'dng warnbtn':'dng')}${actBtn('só worktrees','disk.clean',{mode:'worktrees'},'dng')}</div>`;
  $('disk').classList.toggle('warn',warn);$('disk').classList.toggle('crit',!!crit);
  $('syssum').textContent=sysSummary(s);$('syssum').className='hint '+(crit?'crit':warn?'warn':'')}
