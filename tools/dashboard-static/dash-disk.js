/* painel de disco: o drive do repositório (alerta abaixo de 30 GB livres, crítico abaixo de 10 GB),
   o uso do Docker e os maiores consumidores do repo; o servidor mede o que é lento em segundo plano */
const GIB=1024**3;
const bytes=n=>n==null||n<0?'?':n>=1e12?(n/1e12).toFixed(2)+' TB':n>=1e9?(n/1e9).toFixed(1)+' GB':n>=1e6?Math.round(n/1e6)+' MB':Math.round(n/1e3)+' kB';
const DOCKER_PT={'Images':'Imagens','Containers':'Containers','Local Volumes':'Volumes','Build Cache':'Cache de build'};

function driveCard(d){
  if(!d||!d.ok)return `<div class="dcard"><b>Drive</b><span class="empty">${E((d&&d.error)||'sem dado')}</span></div>`;
  const usedPct=d.total?Math.round(d.used/d.total*100):0,warn=d.level!=='ok';
  return `<div class="dcard ${d.level}"><b>Drive do repositório <small>${E(d.path)}</small></b>
    <div class="dfree">${bytes(d.free)} livres <small>de ${bytes(d.total)}</small></div>
    <div class="pbar"><i style="width:${usedPct}%"></i></div>
    <small>${d.level==='crit'?'CRÍTICO: menos de 10 GB livres; o disco cheio já derrubou o daemon':warn?'Atenção: menos de 30 GB livres':'espaço suficiente'}</small></div>`}

function dockerCard(k){
  if(!k||!k.ok)return `<div class="dcard"><b>Docker</b><span class="empty">${E((k&&k.error)||'sem dado')} (o painel segue sem ele)</span></div>`;
  const rows=(k.rows||[]).map(r=>`<div class="trow"><span class="tt">${E(DOCKER_PT[r.type]||r.type)}</span><small>${r.active} de ${r.count} em uso</small><b>${bytes(r.size)}</b><small>${bytes(r.reclaimable)} recuperáveis</small></div>`).join('');
  return `<div class="dcard"><b>Docker <small>docker system df · lido há ${dur(Math.round(k.age||0))}</small></b>${rows}</div>`}

function repoRows(title,g){
  const top=(g.top||[]).map(x=>`<div class="trow"><span class="tt mono">${E(x.name)}</span><b>${bytes(x.size)}</b></div>`).join('');
  return `<div class="dsub"><b>${title}</b> · ${g.count} · ${bytes(g.size)}</div>${top}`}

function repoCard(r){
  if(!r||!r.ok)return `<div class="dcard"><b>Maiores consumidores</b><span class="empty">${E((r&&r.error)||'sem dado')}</span></div>`;
  return `<div class="dcard"><b>Maiores consumidores <small>medido há ${dur(Math.round(r.age||0))}${r.partial?' · parcial (tempo esgotado)':''}</small></b>
    ${repoRows('.claude/worktrees',r.worktrees)}${repoRows('build-*/',r.builds)}</div>`}

function renderDisk(s){
  const d=s.disk||{};
  $('disk').innerHTML=driveCard(d.drive)+dockerCard(d.docker)+repoCard(d.repo);
  const dr=d.drive;
  $('diskhint').textContent=dr&&dr.ok?bytes(dr.free)+' livres':'';
  $('diskhint').className='hint '+(dr&&dr.ok?dr.level:'')}
