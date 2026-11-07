const hhmm=t=>new Date(t*1000).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'});
const mins=sec=>sec==null?'?':sec<90?Math.round(sec)+' s':Math.round(sec/60)+' min';
function timeCard(s){
  const bt=s.buildtime,l=bt&&bt.lot;
  if(!l)return '<div class="mrow free"><b>Sem build rodando.</b> Quando um lote começar, as etapas, o tempo e a previsão aparecem aqui.</div>';
  const now=Date.now()/1000,pct=l.progress==null?null:Math.round(l.progress*100);
  const chips=l.steps.map(x=>{const el=x.status==='rodando'?Math.max(0,now-(now-(x.dur||0))):x.dur;
    const detail=x.status==='feito'||x.status==='falhou'?mins(x.dur)+(x.avg?' (média '+mins(x.avg)+')':''):x.status==='rodando'?mins(x.dur)+(x.avg?' de ~'+mins(x.avg):' (sem histórico)'):(x.avg?'~'+mins(x.avg):'sem histórico');
    return `<div class="stp ${x.status.replace(' ','')}"><b>${E(x.name)}</b><span>${x.status==='rodando'?'rodando agora':x.status}</span><small>${E(detail)}</small></div>`}).join('');
  const nx=bt.next;
  return `<div class="tcard"><div class="thead"><b>Lote em andamento${l.prs.length?': '+l.prs.map(n=>'#'+n).join(' '):''}</b><span>decorrido ${mins(l.elapsed)}</span><span class="eta">${l.eta?'termina ~'+hhmm(l.eta):'previsão: sem histórico'}</span>${l.missing.length?`<small>sem histórico para: ${E(l.missing.join(', '))} (a previsão não conta essas etapas)</small>`:''}</div>
    <div class="pbar"><i style="width:${pct==null?Math.round(100*l.steps.filter(x=>x.status==='feito').length/l.steps.length):pct}%"></i></div><div class="pl">${pct==null?'barra pelas etapas feitas':pct+'% estimado'} · ${l.steps.filter(x=>x.status==='feito').length} de ${l.steps.length} etapas feitas</div>
    <div class="stps">${chips}</div>
    ${nx?`<div class="tnext">Próximo lote${nx.prs.length?' ('+nx.prs.map(n=>'#'+n).join(' ')+')':''}: ${nx.start?'começa ~'+hhmm(nx.start):'começa quando este terminar (sem previsão por falta de histórico)'}${nx.start?(nx.end?', termina ~'+hhmm(nx.end):' (sem histórico da duração de um lote)'):''}</div>`:''}</div>`}
function renderMachine(s){
  const lock=s.lock||[],now=Date.now()/1000;
  const tc=timeCard(s);
  if(!lock.length){$('mach').innerHTML=tc+'<div class="mrow free"><b>Máquina livre.</b> Nenhum build rodando.</div>';return}
  const idOf=r=>{const a=s.agents.find(a=>a.label===r.who);return r.builder?['builder','builder']:a?['agent',a.id]:['',''] };
  $('mach').innerHTML=tc+lock.map((r,i)=>{const [k,id]=idOf(r);
    return `<div class="mrow ${r.role} ${k?'clk':''}" ${k?`data-k="${k}" data-id="${E(id)}"`:''}><span class="mn">${r.role==='holds'?'Rodando agora':i+'º na fila'}</span><span class="mw">${E(r.who)}</span><span class="mt">${r.role==='holds'?'há ':'espera há '}${dur(Math.round(now-r.start))}</span><span class="mc">${E(r.cmd.replace(/^bash\s+\S*\//,'').slice(0,70))}</span></div>`}).join('')}
function lockRows(lock){
  if(!lock||!lock.length)return '<div class="empty">Ninguém usa a máquina agora.</div>';
  const now=Date.now()/1000;
  return '<div class="lk head"><span>papel</span><span>quem</span><span>prio</span><span>desde</span><span>comando</span></div>'+lock.map(r=>
    `<div class="lk ${r.role}" title="${E(r.cwd)}&#10;${E(r.cmd)}"><span class="rl">${r.role==='holds'?'usando':'espera'}</span><span class="who" title="${E(r.who)}">${E(r.who)}</span><span>prio ${r.prio}</span><span>${dur(Math.round(now-r.start))}</span><span class="cm">${E(r.cmd)}</span></div>`).join('')}
