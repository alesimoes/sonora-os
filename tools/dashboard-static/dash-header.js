function clock(){const d=new Date();$('clock').textContent=d.toLocaleTimeString('pt-BR');$('date').textContent=d.toLocaleDateString('pt-BR',{weekday:'short',day:'2-digit',month:'short',year:'numeric'})}

/* medidores: o DOM e criado uma vez e so os atributos mudam, para a transicao rodar */
const GDEF=[['gw','Semana'],['g5','5 horas'],['gc','Contexto']];
$('usage').innerHTML=GDEF.map(([id,l])=>`<div class="g" id="${id}"><svg viewBox="0 0 60 60"><circle class="bg" cx="30" cy="30" r="25"/><circle class="fg" cx="30" cy="30" r="25" pathLength="100" stroke-dasharray="0 100" transform="rotate(-90 30 30)"/><text x="30" y="38">-</text></svg><div class="gt"><span class="gl">${l}</span><small>&nbsp;</small></div></div>`).join('');
function setGauge(id,pct,sub){const g=$(id),ok=typeof pct==='number';const p=ok?Math.max(0,Math.min(100,pct)):0;
  g.querySelector('.fg').setAttribute('stroke-dasharray',p+' 100');g.querySelector('text').textContent=ok?Math.round(pct):'-';
  g.classList.toggle('hi',ok&&pct>=80&&pct<95);g.classList.toggle('crit',ok&&pct>=95);
  g.querySelector('small').textContent=ok?sub||'':'sem dado'}
function renderUsage(u){const ses=u.sessions||[];const main=ses.find(x=>/sound/i.test(x.cwd||''))||ses[0];
  setGauge('gw',u.week?.used_percentage,u.week?.resets_at?'renova em '+until(u.week.resets_at):'');
  setGauge('g5',u.five_hour?.used_percentage,u.five_hour?.resets_at?'renova em '+until(u.five_hour.resets_at):'');
  setGauge('gc',main?.context_pct,main?((main.model||'')+(main.context_tokens?' · '+Math.round(main.context_tokens/1000)+'k':'')):'')}

