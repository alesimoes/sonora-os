function render(s){
  lastState=s;
  renderSit(s);renderPeople(s);renderMachine(s);renderFlow(s);renderDisk(s);renderTech(s);
  diff(s);
  renderUsage(s.usage||{});
  $('links').innerHTML=(s.links||[]).map(l=>`<a href="${E(l.url)}" target="_blank" rel="noopener">${E(l.title)}</a>`).join('');
  $('err').textContent=(s.errors||[]).join(' | ');
}
async function tick(){
  try{const r=await fetch('/api/state',{cache:'no-store'});const s=await r.json();lastOk=Date.now();render(s);
    $('sub').textContent='atualizado '+new Date().toLocaleTimeString('pt-BR');}
  catch(e){$('err').textContent='sem resposta do servidor: '+e}
}
function pulse(){const age=(Date.now()-lastOk)/1000,stale=!lastOk||age>15;
  $('dot').classList.toggle('stale',stale);
  $('livetxt').textContent=stale?'sem sincronia':'ao vivo · '+Math.round(age)+' s'}
clock();tick();setInterval(tick,5000);setInterval(()=>{pulse();clock()},1000);
