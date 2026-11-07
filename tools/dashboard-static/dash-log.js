/* log de eventos e PRs que acabaram de sair da lista */
let prev=null;const logLines=[];let gone=[];
function ev(cls,txt){const t=new Date().toLocaleTimeString('pt-BR');logLines.unshift(`<div class="${cls}"><time>${t}</time>${E(txt)}</div>`);if(logLines.length>40)logLines.pop();$('log').innerHTML=logLines.join('')}
function diff(s){
  const cur={ag:Object.fromEntries(s.agents.map(a=>[a.id,a])),pr:Object.fromEntries(s.prs.map(p=>[p.number,p]))};
  if(prev){
    for(const id in cur.ag){const a=cur.ag[id],o=prev.ag[id];
      if(!o)ev('info','agente novo: '+a.label);
      else if(o.state!==a.state){
        if(a.state==='done')ev('ok','agente terminou: '+a.label);
        else if(a.state==='waiting'||a.state==='waiting_lock')ev('warn',(a.state==='waiting_lock'?'espera a máquina: ':'agente parado: ')+a.label);
        else ev('info',a.label+' → '+(NAME[a.state]||a.state))}}
    for(const n in cur.pr){const p=cur.pr[n],o=prev.pr[n];
      if(!o)ev('info','PR #'+n+' aberto: '+p.title);
      else if(p.failed.length&&!o.failed.length)ev('bad','PR #'+n+' vermelho: '+p.failed.join(', '));
      else if(!p.failed.length&&o.failed.length)ev('ok','PR #'+n+' voltou a verde');}
    for(const n in prev.pr)if(!cur.pr[n]){ev('ok','PR #'+n+' saiu da lista (mergeado ou fechado)');gone.push({n:+n,title:prev.pr[n].title,t:Date.now()})}
  }else ev('info','painel conectado: '+s.agents.length+' agentes, '+s.prs.length+' PRs');
  gone=gone.filter(g=>Date.now()-g.t<15*60e3);
  prev=cur}
