# Sonora Console: especificação

Um único console de comando, centrado numa conversa com o orquestrador, que substitui o painel atual
(`tools/dashboard.html` e `tools/dashboard-static/*`, v2 incluída). Este PR traz só especificação e maquete;
nenhum código em `tools/` muda. Quem constrói: `ui-engineer` (a tela) e o dono do orquestrador (o interpretador de comandos
e a API de ações, seção 8).

Maquete: `index.html` (responsiva, 1 arquivo, sem rede), `celular.html` (moldura 390 x 844 em volta da mesma página).
Renders: `docs/images/dashboard-console/*.png`, gerados por `render.sh` (Edge/Chrome headless).
Parâmetros da URL só da maquete: `pane=chat|agentes|atencao`, `to=<id-do-agente>`, `ok=1`, `direct=1`.
Origem: HTML/CSS/JS escritos do zero, pilha de fontes do sistema, nenhum arquivo externo. Sem marca, logotipo,
nome de personagem ou de filme. Nome do produto: "Sonora Console".

## Por que (decisão do dono, 2026-11-08)

O painel piorou a cada seção acrescentada. Fica **uma** tela: conversa no centro; em volta, o mínimo.
O dono está longe do computador, quase sempre no celular: o celular é o layout principal, o desktop é o mesmo conteúdo em 3 colunas.
Tudo que antes era página (fluxo, freios, progresso, páginas técnicas) **sai**: vira pergunta ao chat ("por que o PR 544 não entrou?", "como está o progresso?").

## Princípios

1. Perguntar em vez de navegar: informação sob demanda vem como resposta curta + cartão, nunca como página nova.
2. Nada roda sem confirmação visível: toda ação é um **cartão de ação** com o que vai acontecer, Confirmar e Cancelar; depois de executada, Desfazer.
3. Estado nunca só por cor: texto ou ícone junto (▲ risco, AGUARDA / EXECUTADO / DESFEITO, "sem sinal 3 min").
4. Mínimo em volta: a faixa de estado, a lista de agentes e a fila de atenção. Nada mais fica fixo na tela.
5. Português simples, frases curtas, o motivo antes do detalhe técnico.
6. Uma mão, polegar: alvos >= 34 px (abas 44 px), composer fixo no rodapé, fonte do campo 16 px (evita zoom do iOS).

## Layout

### Celular (largura <= 900 px; alvo 390 px)

De cima para baixo:

1. **Faixa de estado** (altura ~46 px, fixa): logo (só o ícone), Daemon, Agentes, PRs, Alertas. "Execuções" some abaixo de 900 px.
2. **Abas** (44 px): `Chat` | `Agentes N` | `Atenção N`. A aba Atenção ganha selo âmbar quando N > 0. Padrão: Chat.
3. **Painel ativo** (o resto da altura): Chat (padrão), lista de agentes, ou fila de atenção.
4. **Composer** só na aba Chat: linha "Para: orquestrador" (ou "Para: eng-101" + "voltar ao orquestrador"), carrossel de sugestões
   (chips de rolagem horizontal), campo + Enviar.

Tocar num agente na aba Agentes vai para Chat com o destino dele. Tocar no botão de um item da fila de atenção vai para Chat e
envia o comando correspondente (o cartão de confirmação aparece lá). Nenhuma ação executa direto da fila sem passar pelo cartão.

### Desktop (> 900 px; alvo 1440 x 900)

Grade `300px | 1fr | 320px` sob a faixa de estado (todas as colunas rolam sozinhas): Agentes à esquerda, Chat no centro
(texto e cartões com no máximo 680 px), Precisa de você à direita. Sem abas. Faixa de estado com os 5 cartões, mais larga, inclui "Execuções".

### Faixa de estado (5 células + 2 de números)

| Célula | Valor | Fonte em `status.json` | Cor / regra |
|---|---|---|---|
| Daemon | `● vivo 6s` / `parado` | `orchestrator.updated` | ok < 30 s; warn < 120 s; bad acima; ponto pulsa só com `prefers-reduced-motion: no-preference` |
| Agentes | `5/8` + barra 2 px | `agents[]`, `orchestrator.cap` | ok < 80 %; warn >= 80 %; bad = cap cheio |
| Execuções (só desktop) | `5/6` + barra | contagem de work runs, limite por hora | mesma regra |
| PRs | `7/8` + barra | `prs[]`, freio (8) | mesma regra; bad = freio ligado |
| Alertas | `3` | tamanho da fila de atenção | ok se 0; warn se > 0; bad se algum item é `bad` |
| Concluído | `52%` + barra | `kpis.progress.pct` | sempre ok (é progresso, não alarme) |
| Merges hoje (só desktop) | `5 · 2/h` | `kpis.merges` | neutro |

Tocar numa célula não abre nada; pergunta ao chat (ex.: tocar em PRs pré-preenche "quais PRs estão abertos?" no campo, sem enviar). Exceção só de atalho, nunca obrigatória.

### Números (painel compacto; dono, 2026-11-08)

O dono vê os números importantes sem abrir o chat. No desktop ficam no topo da coluna da esquerda (acima de Agentes); no celular
são a aba **Números** (Chat, Agentes, Números, Atenção). Mesmo estilo escuro; o chat continua sendo a tela principal.

1. **Progresso**: percentual geral e por trilha (Complete DAW, Plugins, OS packaging), com pontos feitos / total e "+N em curso"
   (barra verde = feito, âmbar = em curso). Fonte: `planning/progress.json` em `origin/main`, o mesmo leitor do dashboard antigo
   (`tools/progress.py`, só trabalho mergeado e verificado conta; opcional não entra). Plugins são os módulos `daw.plugins`,
   `daw.native` e `daw.ampcab`; saem de "Complete DAW", então as três trilhas somam o geral. Relido a cada 5 min.
2. **PRs abertos**: lista com número, título, CI (verde / vermelho / rodando; conflito e cancelado contam como vermelho),
   o que o trem faz com o PR (fase do trem e dono do próximo passo) e minutos aberto. Tocar num PR vai ao Chat e envia
   "por que o PR N não entrou?". Máximo de 30.
3. **Merges**: hoje (dia local) e na última hora, contados dos PRs mergeados lidos do GitHub.

Dados: `GET /api/console` ganha `kpis: {progress: {pct, done, doing, total, tracks[{key,title,pct,done,doing,total}]},
prs[{number,title,ci,level,ci_text,train,minutes_open,late,tap}], merges: {today, last_hour}}`. `progress` é `null` se o
catálogo não pôde ser lido. Código: `tools/dashboard_kpis.py` (regras), `tools/dashboard_progress.py` (leitura),
`tools/dashboard-static/console-kpis.js` (tela). Capturas da tela real com números de exemplo (`render_live.py`):
`docs/images/dashboard-console/celular-numeros.png`, `celular-chat-kpis.png`, `desktop-chat-kpis.png`.

### Lista de agentes

Um cartão por agente **em execução**, ordenado por minutos (maior primeiro), sem agrupar por papel:

| Campo | Fonte | Observação |
|---|---|---|
| id (`eng-104`) | `agents[].id` | mono 13 px, 600 |
| modelo real (`opus`, `sonnet`, `haiku`, `fable`) | `agents[].model` | o modelo que de fato roda, não o planejado; cyan, à direita |
| tarefa (`engine.xrun-recovery`) | `agents[].task` | 1 linha com reticências |
| minutos (`73 min`) | `now - agents[].since` | mono; negrito |
| papel | `agents[].agent` | secundário |
| PR (`#528`) | `agents[].pr` | só se existir |
| sem sinal | `now - last_heartbeat` > 120 s | texto vermelho "sem sinal N min" + borda esquerda 3 px `--bad` |

Tocar seleciona o agente: o destino do composer vira ele (cartão com borda e brilho cyan, `aria-pressed`). O botão tracejado
no fim volta ao orquestrador. Mensagem a um agente vai como pedido pelo orquestrador (seção 8, `agent.say`), e a resposta volta
na conversa com o id dele no cabeçalho (borda verde-azulada). Agentes terminados não aparecem; "o que terminou hoje?" é pergunta.

### Fila de atenção ("Precisa de você")

Só itens que exigem o dono. Cada item: título (1 linha), motivo (1 frase), **um** botão. Gravidade `bad` (borda vermelha, botão âmbar) antes de `warn` (borda âmbar), depois por idade.

| Gera item quando | Botão |
|---|---|
| agente sem sinal > 120 s | Reiniciar `<id>` |
| PR verde > 10 min sem merge | Retomar fila |
| decisão do dono pendente (ADR, bloqueio na handoff "waits on the owner") | opção recomendada |
| daemon parado | Reiniciar daemon |
| disco, lock ou limite de uso (quota) em estado crítico | ação segura do alerta |

Vazio: "Nada precisa de você agora." O item some sozinho quando a causa some. Não há "dispensar": se não exige o dono, não entra.

## Conversa

Fluxo vertical, mensagens novas embaixo, rolagem para o fim ao chegar algo (a menos que o dono tenha rolado para cima: então mostra o chip "novas ↓").
Histórico guardado no servidor e recarregado (últimas 200 mensagens); reaberto o console, a conversa continua.

### Tipos de mensagem

| Tipo | Quem | Visual | Conteúdo |
|---|---|---|---|
| `user` | dono | direita, fundo azul-escuro, rótulo âmbar "VOCÊ" (ou "VOCÊ → eng-101") | texto livre |
| `orch` | orquestrador | esquerda, painel, rótulo cyan "ORQUESTRADOR" | 1 a 3 frases; ids em mono cyan |
| `agent` | agente | esquerda, borda verde-azulada, rótulo com o id | resposta ou aviso de um agente |
| `alert` | sistema | fundo âmbar escuro, "▲ ALERTA" | surge sozinho quando um item entra na fila de atenção |
| `system` | sistema | linha fina centralizada, mono 11 px | início de sessão, reconexão, mudança de estado do daemon |

Respostas do orquestrador: no máximo ~60 palavras; o que passar disso vai num cartão de lista. Sempre o motivo primeiro, depois (se couber) a ação proposta.

### Tipos de cartão

| Cartão | Quando | Estados | Botões |
|---|---|---|---|
| **Ação** (`action`) | o pedido muda algo | `AGUARDA` (âmbar) → `EXECUTANDO` (cyan) → `EXECUTADO` (verde) / `FALHOU` (vermelho) / `CANCELADO` / `DESFEITO` (cinza, 60 % de opacidade) | Confirmar + Cancelar; depois Desfazer; em falha: Tentar de novo |
| **Ação com risco** (`action` + `risk`) | para agentes, para tudo, reinicia daemon, apaga lock | igual, cabeçalho "▲ AÇÃO COM RISCO" em âmbar; o rótulo do botão repete o verbo ("Parar #528") | igual |
| **Lista** (`list`) | resposta de leitura com várias linhas (status, PRs, agentes, fila) | selo "SÓ LEITURA" | nenhum; cada linha com id é tocável e pré-preenche o campo |
| **Explicação** (`why`) | "por que...?" | é uma mensagem `orch` seguida, quando há saída, de um cartão de ação | — |

Anatomia do cartão de ação: cabeçalho (rótulo + selo de estado) · título (verbo + alvo) · **passos** (lista mono, 1 a 4 linhas: o que vai rodar, em ordem) ·
**efeito** (1 frase: quem é afetado, o que fica intacto; trechos críticos em âmbar) · rodapé (botões + info: "Nada roda até confirmar" /
"feito às 14:08 · desfazer vale 10 min").

Regras dos cartões:
- Confirmar chama a API de ação (seção 8) com o `action_id` do cartão; o cartão passa a `EXECUTANDO` e depois ao resultado do orquestrador. Um segundo toque não duplica (botão some ao primeiro).
- Desfazer vale 10 min; vencido, o botão some e o rótulo diz "prazo vencido". Ações sem reversão (ex.: merge feito) não têm Desfazer: o cartão diz "não reversível" antes de confirmar.
- Cartão pendente por mais de 15 min fica `EXPIRADO` (cinza); pedir de novo cria outro.
- Ação com risco exige que o botão de confirmar repita o verbo e o alvo; nunca um "OK" genérico.
- Dois cartões pendentes sobre o mesmo alvo: o mais novo substitui o antigo (o antigo vira `CANCELADO`).

### Estados do console

| Estado | Como aparece |
|---|---|
| Conectando | linha `system` "conectando..."; composer ativo, envio fica em fila |
| Daemon parado | célula Daemon vermelha "parado"; item na fila "Reiniciar daemon"; o chat responde só leitura (`status`, `ajuda`) e avisa que ações ficam bloqueadas |
| Sem rede | faixa âmbar fina sob a faixa de estado "sem conexão, reenviando"; mensagens ficam com relógio e reenviam sozinhas |
| Pensando | `orch` com 3 pontos cyan; sem animação se `prefers-reduced-motion: reduce` |
| Não entendi | `orch` curto + 3 sugestões como chips ("Tente: status, pare o #528, ajuda") |
| Vazio (sem histórico) | mensagem `orch` de boas-vindas + chips |

## Comandos que o console entende

Idioma: português; o interpretador (no orquestrador) é tolerante a acento, caixa, "#" opcional, "PR 544" = "#544", "o/a" opcionais. Tudo que não casa cai em "não entendi". Texto livre pode virar LLM no servidor; o contrato abaixo (intenção, alvos, risco) é o que a tela exige.

Colunas: **Intenção** (id para a API), **Exemplos**, **Resposta**. Risco: L = leitura (responde direto), A = ação (cartão com confirmar), R = ação com risco.

| Intenção | Exemplos | Risco | Resposta / cartão |
|---|---|---|---|
| `status` | "status", "como está tudo?", "situação" | L | frase + cartão lista (daemon, agentes, PRs, merges/h, itens de atenção) |
| `why.pr` | "por que o PR 544 não entrou?", "o que trava o #531?" | L | motivo em 1 a 2 frases + (se couber) cartão "Retomar fila de merge" |
| `why.agent` | "o que o eng-104 está fazendo?", "por que o eng-101 demora?" | L | tarefa, minutos, último sinal, branch |
| `list.agents` | "quem está rodando?", "agentes" | L | cartão lista |
| `list.prs` | "quais PRs estão abertos?", "o que está verde?" | L | cartão lista (número, título, estado do gate, idade) |
| `list.attention` | "o que precisa de mim?" | L | repete a fila de atenção |
| `progress` | "como está o progresso?", "quanto falta do mixer?" | L | % geral + por trilha (cartão lista); substitui a página Progresso |
| `brakes` | "por que não lança agente?", "qual o freio?" | L | quais limites estão cheios (cap, execuções/h, PRs abertos) e quando liberam; substitui a página Freios |
| `flow` | "onde está parado o fluxo?" | L | contagem por estágio (fila, construindo, QA, PR aberto) e o gargalo |
| `agent.stop` | "pare o #528", "para o eng-104", "mata o eng-110" | R | cartão: sinalizar encerrar, manter branch, soltar lock |
| `agent.restart` | "reinicia o eng-104" | R | cartão: encerrar e relançar a mesma tarefa |
| `agent.say` | (toque no agente) "muda para a rota B", ou "fala com o eng-101: ..." | A | mensagem vai ao agente via orquestrador; ele responde na conversa |
| `agent.reroute` | "manda o engenheiro do mixer mudar de rota", "troca a tarefa do eng-101 para bus-meters" | R | cartão: pausar passo atual, nova tarefa, branch intacto, trabalho antigo volta à fila |
| `queue.prioritize` | "prioriza o mixer", "sobe o item X" | A | cartão: reordena a fila; ninguém é interrompido |
| `queue.defer` | "deixa o mixer para depois" | A | cartão reverso do anterior |
| `merge.resume` | "retome a fila de merge", "enfileira o #544" | A | cartão com passos e dependências |
| `pause.all` | "para tudo", "pausa tudo" | R | cartão: pausar lançamentos, agentes salvam e encerram, daemon continua |
| `resume.all` | "pode voltar", "libera tudo" | A | cartão: retomar lançamentos |
| `cap.set` | "limite de agentes 6", "sobe o limite de PRs para 10" | R | cartão com valor atual -> novo |
| `daemon.restart` | "reinicia o daemon" | R | cartão; só funciona com a ponte de reinício disponível |
| `decision.accept` | "aceita o padrão A", "aprova o ADR de roteamento" | A | cartão: grava a decisão e libera o trabalho que a esperava |
| `undo` | "desfaz", "volta atrás" | A | desfaz a última ação dentro dos 10 min (cartão "Desfazer: ...") |
| `help` | "ajuda", "o que você faz?" | L | lista curta dos verbos acima |

Desambiguação: se "pare o #528" casa com um PR e com um agente, o cartão nomeia os dois e o título mostra qual vai parar (o agente dono do PR); se houver 2 candidatos reais, o orquestrador pergunta com chips ("eng-104 ou eng-107?"). Alvo sem correspondência: "não achei #999; os abertos são #528, #531, #544."

## Visual (tokens)

Escuro apenas (HUD). Sem tema claro nesta versão: cai fora do escopo pedido pelo dono; um tema claro seria outra decisão.

| Token | Valor | Uso |
|---|---|---|
| `--bg` | #04070d (com gradiente radial #0a1c30 no topo) | fundo |
| `--bg2` / `--panel` / `--panel2` | #070d18 / #09121f / #0c1829 | barra de abas, cartões, cabeçalho de cartão de ação |
| `--line` / `--line2` | #143049 / #1d4263 | linhas finas 1 px / bordas ativas |
| `--text` / `--mute` | #d6e6f6 / #6c88a4 | texto / secundário |
| `--cyan` / `--cyan-d` | #33dcff / #0e3a4a | foco, orquestrador, ação primária, selecionado |
| `--amber` / `--amber-d` | #ffb23a / #3a2807 | atenção, risco, mensagens do dono, aviso |
| `--ok` | #38d996 | saudável, executado |
| `--bad` / `--bad-d` | #ff5c6c / #3a1219 | falha, sem sinal, daemon parado |
| `--glow` | `0 0 14px rgba(51,220,255,.22)` | cartão pendente, selecionado, botão primário (só em elementos acionáveis) |

Tipografia: corpo `system-ui` 15 px (mensagens e botões); números, ids, rótulos, passos e horas em `ui-monospace` (`SFMono-Regular, Consolas, Liberation Mono`),
com `font-variant-numeric: tabular-nums`; rótulos 10 a 11 px, caixa alta, espaçamento 0,12 a 0,16 em. Raio 3 px, linhas de 1 px (contornos finos), brilho só nos acionáveis.
Fontes: apenas as do sistema, sem arquivo para licenciar. Contraste: texto sobre `--panel` >= 4,5:1; `--mute` (#6c88a4) sobre #09121f dá ~4,9:1, só para texto secundário e nunca para o único portador de estado (princípio 3).
Animação: pulso do ponto do daemon, 2 s, desligado com `prefers-reduced-motion: reduce`.

## Fontes de dados e API (para quem constrói)

A tela lê `GET /api/status` (o `status.json` atual: `orchestrator.{updated,cap,brake,version}`, `agents[]`, `prs[]`, `merged[]`) a cada 5 s, ou por SSE se existir.
O chat precisa de endpoints novos, do lado do orquestrador (fora do escopo deste PR):

| Endpoint | Faz |
|---|---|
| `POST /api/chat` `{text, to?}` -> `{messages:[...]}` | interpreta o pedido; devolve mensagens e cartões (`action_id`, título, passos, efeito, `risk`, `undoable`) |
| `POST /api/action/<action_id>/confirm` | executa; devolve estado |
| `POST /api/action/<action_id>/cancel` | cancela |
| `POST /api/action/<action_id>/undo` | desfaz dentro de 10 min |
| `GET /api/chat?since=<id>` | histórico e mensagens novas (alertas e respostas de agentes) |
| `GET /api/attention` | fila de atenção (derivada de `status.json` + decisões pendentes) |

Segurança: o console escuta fora de `localhost` só por trás do acesso remoto que o dono já usa; toda rota de ação exige token de sessão
e o `action_id` é de uso único (evita repetir um cartão). Nenhuma ação roda por `POST /api/chat`: só `confirm` executa.

## O que o ui-engineer constrói

1. Uma página `tools/dashboard.html` nova (ou substituta) com os 3 painéis, faixa de estado e composer, seguindo `index.html` e os tokens acima; remove as vistas Visão geral, Agentes, PRs, Freios e Progresso.
2. Componentes: `StatusStrip`, `AgentList`, `AttentionQueue`, `Conversation`, mensagens (5 tipos), `ActionCard` (máquina de estados da seção "Tipos de cartão"), `ListCard`, `Composer` (destino, chips, campo).
3. Máquina de estados do cartão: `pending -> running -> done | failed`, `pending -> cancelled | expired`, `done -> undone`.
4. Comportamento no celular: abas, destino ao tocar num agente, tocar em item de atenção vai ao chat com o comando.
5. Testes de tela a 390 x 844 e 1440 x 900 (capturas), com os 5 estados de console acima.

O `orchestrator` (outro agente) entrega o interpretador de comandos e a API.

## Não verificado

- Renderização em Safari iOS e Chrome Android reais: só Edge headless numa moldura de 390 px. `100dvh` e teclado virtual (o composer deve ficar acima do teclado) precisam de teste em aparelho.
- Fontes: pilha do sistema; a aparência de números mono muda por aparelho (Consolas, SF Mono, Roboto Mono). Sem arquivos de fonte, nada a licenciar.
- Custo de `box-shadow` (brilho) em muitos cartões de ação visíveis ao mesmo tempo, em celular fraco: não medido; o brilho só existe em cartões pendentes e selecionados, no máximo 2 ou 3 por vez.
- Interpretação dos comandos: a maquete usa expressões regulares de mentira; a qualidade real do interpretador em português é do orquestrador.
- Os números e nomes da maquete são de exemplo, não dados reais.
