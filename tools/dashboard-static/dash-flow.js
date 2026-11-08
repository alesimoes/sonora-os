/* ---- quadro de fluxo: uma linha por item de trabalho, colunas = fases ---- */
const PH=[['Planejado','ainda sem agente'],['Construindo','agente escreve o código'],['Checks no GitHub','PR aberto, testando'],['Corrigindo','falha ou conflito'],['Pronto p/ merge','tudo verde'],['Na main','já entrou']];
const shortTitle=t=>{t=String(t||'');const m=t.match(/^Bundle ([\w-]+):\s*(.*)$/);if(m)return 'Pacote '+m[1]+' ('+(m[2].match(/#\d+/g)||[]).length+' PRs)';return t.length>78?t.slice(0,76)+'…':t};
function who(role,sub,t0,opts){opts=opts||{};return {role,sub,t0,seed:opts.seed||(role+'|'+sub),code:opts.code||avatarOf(role),opus:!!opts.opus,k:opts.k,id:opts.id,dim:!!opts.dim}}
function whoHtml(w){const now=Date.now()/1000;
  return `<div class="pp ${w.dim?'dim':''}" ${w.k?`data-k="${w.k}" data-id="${E(w.id)}"`:''}><span class="pa">${pixelAvatar(w.code,w.opus,48,w.seed)}</span><span class="pn"><b>${E(w.role)}</b><small>${E(w.sub||'')}${w.t0?(w.sub?' · ':'')+'há '+dur(Math.round(now-w.t0)):''}</small></span></div>`}
function flowRows(s){
  const now=Date.now()/1000,o=s.orchestrator||{};
  const trn=Object.fromEntries((o.train||[]).map(r=>[r.pr,r]));
  const runs=o.runs||[],runByName=Object.fromEntries(runs.map(r=>[r.name,r]));
  const sess=s.agents.filter(a=>!['done','paused'].includes(a.state));
  const used=new Set(),rows=[];
  const roleOf=r=>String(r||'').replace(/^(.*?)(-\d+)?$/,'$1');
  const mkSess=a=>{used.add(a.id);return who(roleName(a.agent),'sessão · '+(a.model||''),a.lock?a.lock.start:a.since_src==='worktree'?0:a.since,{opus:String(a.model).toLowerCase()==='opus',k:'agent',id:a.id,code:avatarOf(a.agent),seed:a.id})};
  s.prs.forEach(p=>{
    const t=trn[p.number],ts=t?t.updated:null,st=prState(p);
    const col=st.col,pend=st.hold?st.text:null,w=[];
    if(col===2)w.push(who('CI do GitHub',st.text,null,{code:'BD',dim:true}));
    if(col===3){const f=t&&t.fixer&&runByName[t.fixer];
      if(t&&t.fixer)w.push(who(roleName(roleOf(t.fixer)),'orquestrador · '+t.fixer,f?f.started:ts,{k:f?'run':'pr',id:f?f.id:p.number}));
      else w.push(who(roleName(t&&t.agent),st.text+' · aguardando agente',ts,{dim:true,k:'pr',id:p.number}))}
    if(col===4)w.push(who('Orquestrador',st.text,ts,{code:'OR',dim:true}));
    sess.filter(a=>a.pr===p.number||(a.branch&&a.branch===p.branch)).forEach(a=>w.push(mkSess(a)));
    rows.push({k:'pr',id:p.number,num:p.number,acts:prActs(p.number,col===4?'verde':'',t),why:p.why,whyN:p.number,title:shortTitle(p.title),sub:(t&&t.track?t.track+' · ':'')+p.branch,col,w,pend,t0:ts,bad:st.cls==='bad'})});
  // trabalho da sessão sem PR: validação, empacotamento etc.
  sess.filter(a=>!used.has(a.id)).forEach(a=>{
    const col=1,pend=null;
    rows.push({k:'agent',id:a.id,num:'',title:a.label,sub:'sessão do coordenador',col,w:[mkSess(a)],pend,t0:a.lock?a.lock.start:null})});
  // features do orquestrador sem PR
  const paused=(o.paused||[]);
  (o.features||[]).filter(f=>!f.pr&&!['merged','blocked','closed'].includes(f.state)).forEach(f=>{
    const planning=['planning','queued'].includes(f.state),col=planning?0:1;
    const ag=(f.tasks||[]).map(t=>t.agent).filter(Boolean)[0];
    rows.push({k:'feature',id:f.id,num:'',title:f.name||f.id,sub:(f.track||'')+(f.adr?' · ADR-'+f.adr:''),col,planned:planning,pend:planning&&(paused.includes('plan')||paused.includes('build'))?'parado: filas pausadas':null,
      w:planning?[who('Arquiteto','orquestrador · aguardando vaga',null,{dim:true,code:'AR',seed:f.id})]:[who(roleName(ag),'orquestrador',f.age?now-f.age:null,{k:'feature',id:f.id,seed:f.id})]})});
  return rows}
function renderFlow(s){
  const rows=flowRows(s),act=rows.filter(r=>!r.planned),plan=rows.filter(r=>r.planned);
  const head=`<div class="fr fh"><div class="ft">O que estamos construindo</div>${PH.map((x,i)=>`<div class="fp"><b>${x[0]}</b><small>${x[1]}</small></div>`).join('')}</div>`;
  const row=r=>`<div class="fr ${r.pend?'hold':''} clk" tabindex="0" data-k="${r.k}" data-id="${E(r.id)}"><div class="ft">${r.num?`<a class="prn" href="${PRURL}${r.num}" target="_blank" rel="noopener">#${r.num}</a> `:''}<span class="ftt">${E(r.title)}</span><div class="fs">${E(r.sub||'')}</div>${r.acts?`<div class="ra">${r.acts}</div>`:''}${r.pend?`<div class="fw">${E(r.pend)}</div>`:''}${r.why?whyHtml(r.why,r.whyN):''}</div>`
    +PH.map((x,i)=>i===r.col?`<div class="fc2 cur ${r.pend?'hold':''}">${r.w.map(whoHtml).join('')||'<span class="none">sem agente</span>'}</div>`:`<div class="fc2 ${i<r.col?'past':'next'}">${i<r.col?'✓':''}</div>`).join('')+'</div>';
  const mg=(s.merged||[]).slice(0,5).map(m=>`<div class="fr done clk" tabindex="0" data-k="pr" data-id="${m.number}"><div class="ft"><a class="prn" href="${PRURL}${m.number}" target="_blank" rel="noopener">#${m.number}</a> <span class="ftt">${E(shortTitle(m.title))}</span></div>${PH.map((x,i)=>i===5?`<div class="fc2 cur ok">entrou às ${clk(Date.parse(m.mergedAt)/1000)}</div>`:'<div class="fc2 past">'+(i<5?'✓':'')+'</div>').join('')}</div>`).join('');
  $('prhint').textContent=`${act.length} em andamento · ${act.filter(r=>r.bad).length} falhando · ${act.filter(r=>r.col===4).length} prontos · ${(s.merged||[]).length} entraram hoje`;
  $('flow').innerHTML=head+(act.map(row).join('')||'<div class="empty">Nada em andamento.</div>')+(mg?'<div class="fsep">Entraram na main hoje</div>'+mg:'')
    +(plan.length?`<details class="fplan" data-key="fplan"><summary>Planejado, ainda sem agente: ${plan.length} (${(s.orchestrator&&(s.orchestrator.paused||[]).length)?'orquestrador pausado nessas filas':'esperando vaga'})</summary>${plan.map(row).join('')}</details>`:'')}
