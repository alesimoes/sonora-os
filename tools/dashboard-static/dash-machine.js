const hhmm=t=>new Date(t*1000).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'});
const mins=sec=>sec==null?'?':sec<90?Math.round(sec)+' s':Math.round(sec/60)+' min';
function renderMachine(s){
  const lock=s.lock||[],now=Date.now()/1000;
  const tc='';
  if(!lock.length){$('mach').innerHTML=tc+'<div class="mrow free"><b>Máquina livre.</b> Nenhum build rodando.</div>';return}
  const idOf=r=>{const a=s.agents.find(a=>a.label===r.who);return a?['agent',a.id]:['',''] };
  $('mach').innerHTML=tc+lock.map((r,i)=>{const [k,id]=idOf(r);
    return `<div class="mrow ${r.role} ${k?'clk':''}" ${k?`data-k="${k}" data-id="${E(id)}"`:''}><span class="mn">${r.role==='holds'?'Rodando agora':i+'º na fila'}</span><span class="mw">${E(r.who)}</span><span class="mt">${r.role==='holds'?'há ':'espera há '}${dur(Math.round(now-r.start))}</span><span class="mc">${E(r.cmd.replace(/^bash\s+\S*\//,'').slice(0,70))}</span>${actBtn('remover da fila','lock.remove',{pid:+r.pid},'dng')}</div>`}).join('')}
function lockRows(lock){
  if(!lock||!lock.length)return '<div class="empty">Ninguém usa a máquina agora.</div>';
  const now=Date.now()/1000;
  return '<div class="lk head"><span>papel</span><span>quem</span><span>prio</span><span>desde</span><span>comando</span></div>'+lock.map(r=>
    `<div class="lk ${r.role}" title="${E(r.cwd)}&#10;${E(r.cmd)}"><span class="rl">${r.role==='holds'?'usando':'espera'}</span><span class="who" title="${E(r.who)}">${E(r.who)}</span><span>prio ${r.prio}</span><span>${dur(Math.round(now-r.start))}</span><span class="cm">${E(r.cmd)}</span></div>`).join('')}
