# Console de operações v2: especificação

Redesenho do painel de agentes (`tools/dashboard.html` + `tools/dashboard-static/*`, porta 8765).
Este PR traz só especificação e maquete; nenhum código do painel muda. Quem constrói: `ui-engineer`.

Maquete clicável: `index.html` (desktop 1080p), `celular.html` (520 px). Dados de exemplo em `console.js`.
Renders: `docs/images/dashboard-v2/<vista>-<escuro|claro|celular>.png`, gerados por `render.sh`
(Edge/Chrome headless). Origem: tudo original, CSS/JS escritos do zero; sem fontes, imagens ou rede externa
(pilha de fontes do sistema). Sem marca, logotipo ou nome de terceiros.

## Princípios

1. Leitura de relance: a faixa de saúde responde "está tudo bem?" em 2 s; o resto responde "onde dói e o que clico".
2. Estado nunca só por cor: todo selo (`.tag`) tem ícone + texto (✔ OK, ▲ Atenção, ✖ Falha, ● Em curso, ○ Inativo).
3. Cada problema traz o **motivo em português simples** e **uma ação** que o resolve.
4. Escuro e claro com os mesmos tokens; contraste de texto >= 4,5:1 nos dois temas.
5. Um layout fluido: 6 colunas de saúde em 1080p, 3 em <=1280 px, 2 em <=820 px; tabelas viram cartões no celular.

## Tokens (CSS custom properties em `console.css`)

| Token | Escuro | Claro | Uso |
|---|---|---|---|
| `--bg` | #0d1117 | #eef1f5 | fundo |
| `--panel` / `--panel2` | #151b23 / #1b232d | #fff / #f4f6f9 | cartão / cartão aninhado |
| `--line` | #283241 | #d5dce6 | bordas |
| `--text` / `--mute` | #e8edf4 / #93a0b2 | #16202c / #566377 | texto / secundário |
| `--accent` | #5eaaff | #0b63c5 | foco, ação, aba ativa |
| `--ok` `--ok-bg` | #3fd08f `#0f2a21` | #0b7a4b `#dcf4e8` | saudável, mergeado |
| `--warn` `--warn-bg` | #f4b942 `#33270c` | #8a5a00 `#fdf0cf` | atenção, parado |
| `--bad` `--bad-bg` | #ff7070 `#37161a` | #b3261e `#fde4e2` | falha, freio ligado |
| `--info` `--info-bg` | #5eaaff `#112640` | #0b63c5 `#dcebfc` | em curso |
| `--idle` `--idle-bg` | #93a0b2 `#1d2530` | #566377 `#e9edf2` | inativo, rascunho |

Tipografia: corpo 15 px (14 px no celular), números-chave 1,45 rem/700, rótulos 0,78 rem caixa alta;
`system-ui` e `ui-monospace` para ids. Raio 10 px, espaçamento 14 px. Alvos de toque >= 32 px no celular.
Animação (`.pulse`) só com `prefers-reduced-motion: no-preference`.

## Estrutura

Barra fixa: marca, abas (Visão geral, Agentes, PRs, Freios e alertas, Progresso; contadores nas abas),
alternador de tema (guardado em `localStorage`, padrão `prefers-color-scheme`), nota "dados de exemplo" na maquete.
Logo abaixo, a **faixa de saúde** fica fixa em todas as vistas.

### Faixa de saúde (6 cartões, borda esquerda 5 px na cor do estado)

| Cartão | Valor | Subtexto / selo | Origem em `status.json` | Regra de estado |
|---|---|---|---|---|
| Daemon | Vivo / Parado | atualizou há N s, versão | `orchestrator.updated`, `orchestrator.version` | ok < 30 s; warn < 120 s; bad acima |
| Agentes | N de cap | trabalhando agora | `agents[].state`, `orchestrator.cap` | ok < 80 %; warn >= 80 %; bad = cap cheio e fila |
| Execuções na hora | N de limite | planners x de y | `orchestrator` (contagem de work runs, limite) | warn >= 80 %; bad = limite |
| PRs abertos | N de freio (8) | "freio ligado: sem lançamentos" | `prs[]`, `orchestrator.brake` | bad quando N >= freio |
| Alertas | N | quantos exigem ação | derivado (stall, pausas, disco, lock) | bad se algum exige ação |
| Throughput | N /h | merges hoje | `merged[]` | informativo |

Barra de progresso de 6 px nos cartões com limite.

### Visão geral
- **Fluxo do trabalho**: 7 estágios em fila horizontal com chevrons: Na fila, Planejando, Construindo, QA, Integrando, PR aberto, Na main.
  Cada estágio: contagem grande, nome, dica de uma linha. Estágio travado = fundo `--warn-bg` + selo "Parado"; com falha = `--bad-bg` + "Travado". Último estágio em `--ok-bg`.
  Abaixo: entradas na última hora, merges hoje, tempo médio até o merge, mini-gráfico de merges por hora (12 h).
- **Onde está parado**: lista de travas com selo, título ("PR aberto: #512 e #517 verdes há mais de 10 min sem merge"), **Motivo:** em português e um botão de ação.
- **Alertas e freios** (resumo) e **Progresso** por trilha.

### Agentes
Cartões agrupados por papel (cabeçalho "Engenheiro de interface · 3 ativos"), grade `auto-fill minmax(310px,1fr)`.
Cartão: avatar com sigla, nome, papel, selo (Trabalhando / Duplicado no PR / Sem sinal), título da tarefa,
lista Feature, PR, Tempo, Sinal (10 barras de batimento + "há N s"), Lançado por (daemon ou coordenador),
ações Ver detalhe e Parar. Duplicado no mesmo PR: borda `--warn` e aviso interno nomeando o outro agente.
Sem sinal > 120 s: borda `--bad`. Finalizados: lista compacta recente ao fim (id, papel, resultado, duração).
Origem: `agents[]` (`id`, `agent`, `model`, `state`, `since`, `since_src`, `pr`, `branch`, `lock`).

### PRs
Quadro de 4 colunas: Rascunho / aguardando, Checks rodando, Prontos para merge, Mergeados hoje.
Cartão: `#número` mono, selo de Gate (Rascunho, Bloqueado, Checks rodando 9 de 14, Verde, Falhou), título, trilha, idade, dono do merge (agente, orquestrador, dono do projeto).
**Verde há mais de 10 min sem merge**: borda e faixa esquerda `--ok` mais selo "verde há N min"; a ação "Retomar fila merge" ou "Mesclar" aparece no cartão.
Origem: `prs[]` (`number`, `title`, `branch`, `draft`, `pending`, `passed`, `total`, `failed`, `cancelled`, `mergeable`, `merge`, `why`).
Celular: uma coluna, cada coluna é um cartão recolhível.

### Freios e alertas
Cartão por razão, em ordem de gravidade, com texto simples e **uma** ação:

| Razão | Texto | Ação que libera |
|---|---|---|
| PRs abertos no limite | "8 de 8 PRs abertos. Nenhum item novo é lançado até um PR entrar na main." | Ver PRs prontos |
| Limite por hora | "14 de 20 execuções na última hora. Folga de 6." | nenhuma (informativo) |
| Teto de agentes | "10 de 10 agentes; novos esperam vaga." | Ver agentes |
| Fila pausada | "Fila 'merge' pausada há 22 min; 2 PRs verdes esperam." | Retomar fila merge |
| Piscina pausada | "A piscina 'builders' foi pausada à mão." | Retomar piscina |
| Reivindicações (claims) | "Dois agentes querem os mesmos arquivos." | Ver agentes |
| Disco / lock | "Disco com 6 % livre" / "Lock preso por processo morto" | Abrir limpeza / Liberar lock |

Ações destrutivas (Parar, Liberar lock) pedem confirmação; endpoints existentes de `dash-actions.js`
(`adopt`, `merge`, `regate`, `rerun_step`, `assign`), sem endpoint novo.

### Progresso
Uma barra por trilha (DAW, Sistema, Plugins): percentual, "N pontos na main, +M em andamento". Origem: `progress`.

## Comportamento

- Atualiza a cada 5 s; `aria-live="polite"` na faixa de saúde; troca de aba por hash (`#agentes`) e teclado (setas na lista de abas).
- Dado velho (>120 s) cinza com selo "desatualizado", nunca vazio.
- Estados vazios com texto ("Nenhum agente trabalhando").

## O que o ui-engineer constrói

1. Substituir o markup e o CSS por `console.css` (tokens e componentes já prontos) reaproveitando os módulos de dados `dash-*.js`.
2. Renderizadores novos: faixa de saúde, fluxo, travas, cartões de agente, quadro de PRs, freios, progresso.
3. Regras de estado das tabelas acima, o destaque "verde > 10 min" e a detecção de duplicado por PR.
4. Tema com `localStorage`; teste de captura de tela das 5 vistas x 3 formatos.

## Não verificado

- Fontes: usa a pilha do sistema, o render mostra Segoe UI/Cascadia; no Linux cai em DejaVu/Roboto.
- Nomes exatos dos campos do orquestrador para limite por hora, freio e teto (inferidos de `dash-strip.js`/`dash-flow.js`); confirmar com o dono do `status.json`.
- Renders só no Edge headless a 1920x1080 e 520x844; sem teste em telefone real nem leitor de tela.
- Contraste calculado a olho nos tokens, sem ferramenta.
