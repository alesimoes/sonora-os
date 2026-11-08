/* Maquete clicável: dados de exemplo fixos (inventados), nenhuma rede. Na versão real os mesmos
   campos vêm de planning/orchestrator/status.json via /api/status. */
const D = {
  daemon: {vivo: true, ha: 8, versao: '4f2a91c', modo: 'ao vivo'},
  agentes: {rodando: 7, teto: 10}, exec: {h: 14, lim: 20, pl: 3, plLim: 6},
  prs: {abertos: 8, freio: 8}, alertas: 4,
  fluxo: [
    ['Na fila', 5, 'esperando vaga', ''], ['Planejando', 1, 'arquiteto escreve o plano', ''],
    ['Construindo', 4, 'agentes escrevem código', ''], ['QA', 1, 'testes e revisão', ''],
    ['Integrando', 1, 'conflito sendo resolvido', 'fail'], ['PR aberto', 8, 'checks e merge', 'stuck'], ['Na main', 12, 'hoje', 'end']],
  vazao: {hora: 3, dia: 12, mediaMerge: '38 min', porHora: [0, 1, 0, 2, 1, 1, 0, 3, 2, 1, 2, 3]},
  travas: [
    ['warn', 'PR aberto', '#512 e #517 verdes há mais de 10 min sem merge', 'orquestrador com a fila “merge” pausada'],
    ['bad', 'Integrando', 'Pacote os-image parado há 42 min', 'conflito em iso/config; integrador sem vaga (teto de agentes)']],
  agentes_lista: [
    {papel: 'Engenheiro de interface', sig: 'UI', nome: 'eng-68', tarefa: 'Redesenhar o painel de operações', feat: 'dash.v2-design', pr: null, t: '23 min', hb: 'há 12 s', por: 'daemon', est: 'ok'},
    {papel: 'Engenheiro de interface', sig: 'UI', nome: 'eng-71', tarefa: 'Régua de tempo com zoom vertical', feat: 'daw.timeline-zoom', pr: 531, t: '1 h 05', hb: 'há 20 s', por: 'daemon', est: 'ok'},
    {papel: 'Engenheiro de interface', sig: 'UI', nome: 'eng-74', tarefa: 'Régua de tempo com zoom vertical', feat: 'daw.timeline-zoom', pr: 531, t: '9 min', hb: 'há 4 s', por: 'coordenador', est: 'dup', dup: 'mesmo PR #531 que eng-71'},
    {papel: 'Engenheiro de tempo real', sig: 'RT', nome: 'rt-12', tarefa: 'Anel de gravação sem bloqueio', feat: 'daw.ring-capture', pr: 528, t: '2 h 14', hb: 'há 41 s', por: 'daemon', est: 'ok'},
    {papel: 'Engenheiro de DSP', sig: 'DS', nome: 'dsp-09', tarefa: 'Compressor óptico, curva de ataque', feat: 'plugin.opto-comp', pr: null, t: '47 min', hb: 'há 3 min', por: 'daemon', est: 'stale'},
    {papel: 'Testador', sig: 'QA', nome: 'qa-05', tarefa: 'Validar pacote de captura', feat: 'daw.ring-capture', pr: 528, t: '6 min', hb: 'há 9 s', por: 'daemon', est: 'ok'},
    {papel: 'Integrador', sig: 'IN', nome: 'int-02', tarefa: 'Resolver conflito do pacote os-image', feat: 'os.image-pack', pr: 519, t: '42 min', hb: 'há 15 s', por: 'coordenador', est: 'ok'}],
  recentes: [
    ['dsp-08', 'Engenheiro de DSP', 'plugin.tape-echo', '#524', 'concluído', 'ok', '14 min'],
    ['rv-03', 'Revisor', 'daw.ring-capture', '#528', 'concluído', 'ok', '31 min'],
    ['eng-66', 'Engenheiro de interface', 'daw.mixer-strip', '#520', 'falhou: teste', 'bad', '58 min'],
    ['eng-65', 'Engenheiro de SO', 'os.usb-boot', '#518', 'concluído', 'ok', '1 h 20']],
  prs_lista: [
    {n: 512, t: 'Faixa do mixer com medidor de pico', trilha: 'daw', gate: 'ok', gtxt: 'Pronto para o merge', dono: 'orquestrador', idade: '2 h 10', verde: '18 min'},
    {n: 517, t: 'Cadeia de inserts por faixa', trilha: 'daw', gate: 'ok', gtxt: 'Pronto para o merge', dono: 'orquestrador', idade: '1 h 40', verde: '12 min'},
    {n: 528, t: 'Anel de gravação sem bloqueio', trilha: 'daw', gate: 'info', gtxt: 'Checks rodando · 9 de 14', dono: 'orquestrador', idade: '34 min'},
    {n: 531, t: 'Régua de tempo com zoom vertical', trilha: 'daw', gate: 'info', gtxt: 'Checks rodando · 3 de 14', dono: 'orquestrador', idade: '9 min'},
    {n: 519, t: 'Pacote os-image: 3 PRs', trilha: 'os', gate: 'bad', gtxt: 'Conflito com a main', dono: 'integrador int-02', idade: '3 h 05'},
    {n: 523, t: 'Eco de fita, painel frontal', trilha: 'plugin', gate: 'bad', gtxt: 'Falhou: ui-tests', dono: 'fixer fx-04', idade: '1 h 12'},
    {n: 526, t: 'Atalhos de edição (modo Slip)', trilha: 'daw', gate: 'idle', gtxt: 'Rascunho: agente ainda escrevendo', dono: 'agente', idade: '25 min'},
    {n: 529, t: 'Notas de versão da imagem', trilha: 'os', gate: 'warn', gtxt: 'Bloqueado: falta revisão', dono: 'dono do projeto', idade: '50 min'}],
  freios: [
    {tipo: 'warn', nome: 'Fila “merge” pausada', pq: 'Alguém pausou a fila há 22 min; 2 PRs verdes esperam.', ac: [['Retomar fila merge', 'primary']]},
    {tipo: 'warn', nome: 'PRs abertos no limite', pq: '8 de 8 PRs abertos. Nenhum item novo é lançado até um PR entrar na main.', ac: [['Ver PRs prontos', '']]},
    {tipo: 'ok', nome: 'Limite por hora', pq: '14 de 20 execuções na última hora. Folga de 6.', ac: []},
    {tipo: 'warn', nome: 'Teto de agentes quase cheio', pq: '7 de 10 agentes; 1 integrador espera vaga para o pacote os-image.', ac: [['Parar agente parado (dsp-09)', 'danger']]},
    {tipo: 'info', nome: 'Reservas de arquivos (claims)', pq: 'eng-71 segura ui/qt/timeline/**; eng-74 pediu o mesmo caminho e espera.', ac: [['Liberar reserva de eng-74', '']]},
    {tipo: 'bad', nome: 'Daemon sem sinal (exemplo, hoje está vivo)', pq: 'Sem atualização do status.json há mais de 120 s: nada é distribuído.', ac: [['Reiniciar daemon', 'danger']]}],
  trilhas: [['daw', 'DAW', 62, 41, 9], ['os', 'Sistema', 38, 22, 3], ['plugin', 'Plugins', 47, 31, 5]],
  itens: [
    ['daw', 'Anel de gravação sem bloqueio', 'Construindo', 70], ['daw', 'Régua de tempo com zoom', 'Construindo', 40],
    ['os', 'Pacote de imagem USB', 'Integrando', 85], ['plugin', 'Compressor óptico', 'Construindo', 30], ['plugin', 'Eco de fita', 'PR aberto', 90]]
};
const $ = s => document.querySelector(s), E = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
const TXT = {ok: 'OK', warn: 'Atenção', bad: 'Falha', info: 'Em andamento', idle: 'Aguardando'};
const tag = (k, t) => `<span class="tag ${k}">${E(t || TXT[k])}</span>`;
const btn = (t, k) => `<button class="btn sm ${k || ''}" type="button">${E(t)}</button>`;

function health() {
  const d = D, pct = (a, b) => Math.round(100 * a / b);
  const c = (cls, k, v, sub, st, m) => `<div class="hc ${cls}"><div class="k">${k}</div><div class="v">${v}</div>${m != null ? `<div class="meter"><i style="width:${m}%"></i></div>` : ''}<div class="s">${tag(cls, st)} <span class="mute">${sub}</span></div></div>`;
  $('#health').innerHTML =
    c('ok', 'Daemon', 'Vivo', `atualizou há ${d.daemon.ha} s · ${d.daemon.versao}`, 'OK') +
    c('warn', 'Agentes', `${d.agentes.rodando} <small>de ${d.agentes.teto}</small>`, 'trabalhando agora', 'Quase no teto', pct(d.agentes.rodando, d.agentes.teto)) +
    c('ok', 'Execuções na hora', `${d.exec.h} <small>de ${d.exec.lim}</small>`, `planners ${d.exec.pl} de ${d.exec.plLim}`, 'Dentro do limite', pct(d.exec.h, d.exec.lim)) +
    c('bad', 'PRs abertos', `${d.prs.abertos} <small>de ${d.prs.freio}</small>`, 'freio ligado: sem lançamentos', 'No freio', 100) +
    c('bad', 'Alertas', d.alertas, '2 exigem ação agora', 'Ação necessária') +
    c('ok', 'Throughput', `${D.vazao.hora}<small> /h</small>`, `${D.vazao.dia} na main hoje`, 'Normal');
}

function flow() {
  const mx = Math.max(...D.vazao.porHora);
  return `<div class="flow" role="list">${D.fluxo.map(s => `<div class="st ${s[3]}" role="listitem"><div class="n">${s[1]}</div><div class="l">${s[0]} ${s[3] === 'stuck' ? tag('warn', 'Parado') : s[3] === 'fail' ? tag('bad', 'Travado') : ''}</div><div class="h">${s[2]}</div></div>`).join('')}</div>
  <div class="tp"><div><b>${D.vazao.hora}</b><span>entraram na última hora</span></div><div><b>${D.vazao.dia}</b><span>na main hoje</span></div><div><b>${D.vazao.mediaMerge}</b><span>tempo médio até o merge</span></div>
  <div><div class="spark" aria-label="merges por hora, últimas 12 horas">${D.vazao.porHora.map(v => `<i style="height:${Math.max(3, 34 * v / mx)}px" title="${v}"></i>`).join('')}</div><span>merges por hora (12 h)</span></div></div>`;
}

function stuck() {
  return D.travas.map(t => `<div class="al ${t[0]}">${tag(t[0])}<div><b>${E(t[1])}: ${E(t[2])}</b><p>Motivo: ${E(t[3])}</p></div><div class="acts">${btn(t[0] === 'bad' ? 'Abrir integrador' : 'Retomar fila merge', 'primary')}</div></div>`).join('');
}

function agentCard(a) {
  const e = a.est, st = e === 'dup' ? tag('warn', 'Duplicado no PR') : e === 'stale' ? tag('bad', 'Sem sinal') : tag('info', 'Trabalhando');
  const beat = [...Array(10)].map((_, i) => `<i class="${e === 'stale' && i > 5 ? 'b' : e === 'stale' && i > 3 ? 'm' : ''}"></i>`).join('');
  return `<article class="ag ${e === 'dup' ? 'dup' : e === 'stale' ? 'stale' : ''}"><header><span class="av" aria-hidden="true">${a.sig}</span><div><b>${E(a.nome)}</b><small>${E(a.papel)}</small></div><span style="margin-left:auto">${st}</span></header>
  <div class="t">${E(a.tarefa)}</div>
  <dl class="kv"><dt>Feature</dt><dd class="mono">${E(a.feat)}</dd><dt>PR</dt><dd>${a.pr ? '#' + a.pr : '<span class="mute">ainda sem PR</span>'}</dd><dt>Tempo</dt><dd>${a.t}</dd><dt>Sinal</dt><dd><span class="beat" aria-hidden="true">${beat}</span> ${a.hb}</dd><dt>Lançado por</dt><dd>${a.por === 'daemon' ? 'daemon' : 'coordenador (manual)'}</dd></dl>
  ${a.dup ? `<div class="al warn" style="grid-template-columns:1fr;margin:0">${tag('warn', 'Duplicado')} <p>${E(a.dup)}. Dois agentes no mesmo PR geram conflito.</p></div>` : ''}
  <footer>${btn('Ver detalhe')}${btn('Parar', 'danger')}${e === 'stale' ? btn('Relançar', 'primary') : ''}</footer></article>`;
}

function agents() {
  const g = {};
  D.agentes_lista.forEach(a => (g[a.papel] = g[a.papel] || []).push(a));
  return Object.entries(g).map(([p, l]) => `<div class="role"><h3>${E(p)}</h3><span class="c">${l.length} ativo${l.length > 1 ? 's' : ''}</span></div><div class="cards">${l.map(agentCard).join('')}</div>`).join('') +
    `<h3 class="sec">Terminaram recentemente</h3><div class="card"><table class="rs"><thead><tr><th>Agente</th><th>Papel</th><th>Feature</th><th>PR</th><th>Resultado</th><th>Há</th></tr></thead><tbody>${D.recentes.map(r => `<tr><td class="mono">${r[0]}</td><td>${r[1]}</td><td class="mono">${r[2]}</td><td>${r[3]}</td><td>${tag(r[5], r[4])}</td><td>${r[6]}</td></tr>`).join('')}</tbody></table></div>`;
}

function prBoard() {
  const col = (t, f) => { const l = D.prs_lista.filter(f); return `<section class="col"><h3>${t}<span class="mute">${l.length}</span></h3>${l.map(p => `<div class="pr ${p.verde ? 'late' : ''}"><div><span class="n">#${p.n}</span> ${tag(p.gate, p.gtxt)}</div><div class="t">${E(p.t)}</div>
    <div class="m"><span>trilha ${p.trilha}</span><span>idade ${p.idade}</span><span>dono: ${E(p.dono)}</span></div>
    ${p.verde ? `<div class="al ok" style="margin:0;grid-template-columns:1fr"><b>Verde há ${p.verde} sem merge</b><p>Limite de atenção: 10 min.</p>${btn('Fazer merge agora', 'danger')}</div>` : ''}</div>`).join('') || '<small>vazio</small>'}</section>`; };
  return `<div class="board">${col('Rascunho / aguardando', p => ['idle', 'warn'].includes(p.gate)) + col('Checks rodando', p => p.gate === 'info') + col('Precisa de correção', p => p.gate === 'bad') + col('Pronto para o merge', p => p.gate === 'ok')}</div>
  <h3 class="sec">Lista</h3><div class="card"><table class="rs"><thead><tr><th>PR</th><th>Título</th><th>Gate</th><th>Dono do merge</th><th>Idade</th><th>Verde sem merge</th></tr></thead><tbody>${D.prs_lista.map(p => `<tr class="${p.verde ? 'hl' : p.gate === 'bad' ? 'hlb' : ''}"><td class="mono">#${p.n}</td><td>${E(p.t)}</td><td>${tag(p.gate, p.gtxt)}</td><td>${E(p.dono)}</td><td>${p.idade}</td><td>${p.verde ? tag('warn', p.verde + ' · destacado') : '<span class="mute">n/a</span>'}</td></tr>`).join('')}</tbody></table></div>`;
}

function brakes(n) {
  return D.freios.slice(0, n || 99).map(f => `<div class="al ${f.tipo}">${tag(f.tipo)}<div><b>${E(f.nome)}</b><p>${E(f.pq)}</p></div><div class="acts">${f.ac.map(a => btn(a[0], a[1])).join('') || '<small>sem ação necessária</small>'}</div></div>`).join('');
}

function trilhas() {
  return D.trilhas.map(t => `<div class="trk"><b>${t[1]}</b><div class="bar2 ${t[2] > 80 ? 'ok' : ''}" role="progressbar" aria-valuenow="${t[2]}" aria-valuemin="0" aria-valuemax="100"><i style="width:${t[2]}%"></i></div><b>${t[2]}%</b></div><small>${t[3]} pontos na main, +${t[4]} em andamento</small>`).join('');
}
function progress() {
  const tr = trilhas();
  return `<div class="grid g2"><div class="card"><h2>Progresso por trilha <small>só conta o que está na main e verificado</small></h2>${tr}</div>
  <div class="card"><h2>Itens em andamento</h2>${D.itens.map(i => `<div class="trk" style="grid-template-columns:1fr 60px"><span>${E(i[1])}<br><small>${i[0]} · ${i[2]}</small></span><b>${i[3]}%</b></div><div class="bar2"><i style="width:${i[3]}%"></i></div>`).join('')}</div></div>`;
}

function build() {
  health();
  $('#v-visao').innerHTML = `<div class="grid g2"><div class="card"><h2>Fluxo do trabalho <small>do pedido até a main</small></h2>${flow()}</div><div class="card"><h2>Onde está parado ${tag('bad', D.travas.length + ' travas')}</h2>${stuck()}</div></div>
   <div class="grid g2" style="margin-top:14px"><div class="card"><h2>Alertas e freios <small>o que segura o sistema</small></h2>${brakes(3)}</div><div class="card"><h2>Progresso</h2>${trilhas()}</div></div>`;
  $('#v-agentes').innerHTML = agents();
  $('#v-prs').innerHTML = prBoard();
  $('#v-freios').innerHTML = `<div class="card"><h2>Por que algo está retido <small>cada freio traz a única ação que o libera</small></h2>${brakes()}<div class="leg"><span>${tag('ok')} dentro do limite</span><span>${tag('warn')} retendo, precisa de decisão</span><span>${tag('bad')} parado</span><span>${tag('info')} informação</span></div></div>`;
  $('#v-progresso').innerHTML = progress();
}

function show(v) {
  document.querySelectorAll('.view').forEach(e => e.classList.toggle('on', e.id === 'v-' + v));
  document.querySelectorAll('.tabs button').forEach(b => b.setAttribute('aria-selected', b.dataset.v === v));
  if (location.hash !== '#' + v) history.replaceState(null, '', '#' + v);
}
function theme(t) { document.documentElement.dataset.theme = t; $('#th').textContent = t === 'dark' ? 'Tema claro' : 'Tema escuro'; }

build();
document.querySelectorAll('.tabs button').forEach(b => b.onclick = () => show(b.dataset.v));
$('#th').onclick = () => theme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark');
theme(new URLSearchParams(location.search).get('tema') === 'claro' ? 'light' : 'dark');
show(location.hash.slice(1) || 'visao');
