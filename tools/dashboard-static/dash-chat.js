function chatHtml(c){if(!c)return '';
  const th=(c.thread||[]).map(m=>`<div class="msg ${m.who==='owner'?'owner':''}"><small>${E(m.who==='owner'?'você':m.who)} · ${E(m.type)} · ${E(m.ts)}</small>${E(m.text)}</div>`).join('')||'<div class="ev">nenhuma mensagem ainda</div>';
  const note=!c.daemon_alive?'o daemon não está rodando: nada será entregue':c.running?'o agente lê o correio entre uma ferramenta e outra':'sem execução ativa: acorde para relançar';
  return `<h4>Conversa com ${E(c.name)}</h4>${th}${c.pending?`<div class="ev">${c.pending} pedido(s) esperando o daemon</div>`:''}
    <textarea id="chatbox" placeholder="O que você está fazendo?" maxlength="2000"></textarea>
    <div class="chatbtns"><button type="button" onclick="chatSend('ask')">perguntar</button><button type="button" onclick="chatSend('wake')">acordar</button><span class="st" id="chatst">${E(note)}</span></div>`}
async function chatSend(kind){const box=$('chatbox'),name=detOpen&&detOpen.id,st=$('chatst');
  const r=await fetch('/api/agent/'+kind,{method:'POST',headers:{'Content-Type':'application/json','X-Dash-Token':DASH_TOKEN},body:JSON.stringify({name:lastChatName,text:box?box.value:''})});
  const j=await r.json();if(r.ok&&kind==='ask'&&box)box.value='';st.textContent=r.ok?'pedido enviado ao daemon':(j.error||r.status);loadDetail()}
let lastChatName='';
