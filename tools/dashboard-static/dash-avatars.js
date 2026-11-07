/* ---- visão simples: frase da situação, agentes como pessoas, máquina, PRs ---- */
const ROLE_PT={'principal-engineer':'Engenheiro principal','rt-engineer':'Engenheiro de tempo real','ui-engineer':'Engenheiro de interface','distro-engineer':'Engenheiro de distribuição','plugin-engineer':'Engenheiro de plugins','dsp-engineer':'Engenheiro de DSP','general-purpose':'Agente geral','fixer':'Corretor','integrator':'Integrador','coordenador':'Coordenador','architect':'Arquiteto','planner':'Planejador','reviewer':'Revisor','qa':'Testador'};
const roleName=r=>ROLE_PT[r]||(r?String(r).replace(/-/g,' ').replace(/^./,c=>c.toUpperCase()):'Agente');
const AVT=[['principal','P'],['coord','C'],['ui-','UI'],['rt-','RT'],['fix','FX'],['integr','IN'],['plugin','PL'],['dsp','DS'],['distro','OS'],['general','AG'],['orch','OR'],['build','BD'],['arch','AR'],['plan','PL'],['review','RV'],['qa','QA']];
const avatarOf=r=>{r=String(r||'').toLowerCase();const m=AVT.find(x=>r.includes(x[0]));return m?m[1]:(r.slice(0,2).toUpperCase()||'?')};
/* rostos em pixel 14x14: o cargo define o acessório, o hash do id/nome varia pele, cabelo, olhos e boca;
   a camisa mostra o modelo (roxo = Opus, verde-azulado = Sonnet) */
const SKINS=['#f2c9a0','#e0ac84','#c68642','#8d5524','#f7d7c4','#a0673c'];
const HAIRS=['#2b2118','#5a3825','#a0522d','#e8b04b','#c9d1d9','#1f1f2e','#ff8fb1','#7a5cff'];
const hash=str=>{let h=2166136261;for(const ch of String(str)){h^=ch.charCodeAt(0);h=Math.imul(h,16777619)}h^=h>>>15;h=Math.imul(h,2246822519);h^=h>>>13;return h>>>0};
let FORCE=null;
const hexRgb=c=>[1,3,5].map(i=>parseInt(c.slice(i,i+2),16));
const rgbHex=a=>'#'+a.map(v=>Math.max(0,Math.min(255,Math.round(v))).toString(16).padStart(2,'0')).join('');
const shade=(c,f)=>rgbHex(hexRgb(c).map(v=>v*f));
const light=(c,f)=>rgbHex(hexRgb(c).map(v=>v+(255-v)*f));
const mix=(a,b,f)=>{const x=hexRgb(a),y=hexRgb(b);return rgbHex(x.map((v,i)=>v+(y[i]-v)*f))};
function pixelAvatar(code,opus,size,seed){
  const h=hash(seed||code),pk=(a,sh)=>a[(h>>>sh)%a.length];
  const F=FORCE||{},skin=F.skin||pk(SKINS,0),H=F.hair||pk(HAIRS,4),style=F.style||pk(['short','long','bald','curly','bun','side'],8),eyec=pk(['#8a5a34','#3d78c9','#3f9a62','#6a7d8f'],12);
  const N=24,g=Array.from({length:N},()=>Array(N).fill(null));
  const set=(x,y,c)=>{if(x>=0&&x<N&&y>=0&&y<N)g[y][x]=c};
  const rect=(x,y,w,hh,c)=>{for(let j=0;j<hh;j++)for(let i=0;i<w;i++)set(x+i,y+j,c)};
  const row=(y,x0,x1,c)=>rect(x0,y,x1-x0+1,1,c);
  const pix=(c,pts)=>pts.forEach(([x,y])=>set(x,y,c));
  const SK2=shade(skin,.88),SK3=shade(skin,.74),SKL=light(skin,.28);
  const HL=light(H,.38),HS=shade(H,.74),HD=shade(H,.55);
  const OUT='#14161b',DK='#1b1d24',WH='#ffffff';
  const shirt=opus?'#a78bfa':'#3fc1b0',SHL=light(shirt,.28),SH2=shade(shirt,.8),SH3=shade(shirt,.62);
  const GOLD='#f2c14e',GOLD2='#c9922c',GOLDL='#fbe6a0';
  // cabelo comprido atrás da cabeça e dos ombros
  if(style==='long'){rect(5,6,14,16,H);rect(5,17,3,5,HS);rect(16,17,3,5,HS);pix(HD,[[5,6],[18,6]])}
  // camisa: ombros, dobra, gola em V, pescoço
  row(18,8,15,shirt);row(19,5,18,shirt);rect(3,20,18,4,shirt);
  rect(17,20,4,4,SH2);pix(SH2,[[16,19],[17,19],[18,19]]);pix(SHL,[[6,19],[5,20],[4,21],[4,22],[3,22]]);
  pix(SH3,[[7,21],[7,22],[8,23],[9,23]]);pix(SH2,[[8,21],[8,22]]);
  rect(10,17,4,2,SK2);pix(SK2,[[11,19],[12,19]]);pix(SK3,[[10,17],[11,17],[12,17],[13,17]]);
  pix('#ffffff',[[9,18],[14,18],[10,19],[13,19]]);pix('#d5dae0',[[8,18],[15,18]]);
  // cabeça: mandíbula, orelhas, sombreado em 3 tons
  row(5,8,15,skin);row(6,7,16,skin);for(let y=7;y<=13;y++)row(y,6,17,skin);row(14,7,16,skin);row(15,8,15,skin);row(16,9,14,skin);
  for(let y=7;y<=13;y++){set(16,y,SK2);set(17,y,SK2)}pix(SK2,[[15,14],[16,14],[14,15],[15,15],[12,16],[13,16],[14,16]]);
  pix(SKL,[[7,9],[7,10],[8,8]]);
  pix(skin,[[5,10],[5,11],[5,12]]);pix(SK2,[[18,10],[18,11],[18,12]]);pix(SK3,[[5,11],[18,11]]);
  if(style!=='bald')row(7,8,15,SK2);
  // cabelo (3 tons: base, brilho, sombra)
  if(style==='short'){row(3,9,14,H);row(4,7,16,H);row(5,6,17,H);row(6,6,17,H);rect(6,7,2,2,H);rect(16,7,2,2,HS);
    row(6,10,13,HS);pix(HL,[[9,4],[10,4],[11,4],[8,5],[9,5]]);pix(HS,[[14,4],[15,4],[16,5],[17,5],[15,3]])}
  if(style==='side'){row(3,9,14,H);row(4,7,16,H);row(5,6,17,H);row(6,6,15,H);rect(6,7,3,2,H);rect(16,6,2,3,HS);row(7,9,10,H);
    pix(HL,[[9,4],[10,4],[11,4],[12,4],[8,5],[9,5],[10,5]]);pix(HS,[[13,4],[14,5],[15,6],[12,6],[13,6],[14,6]]);pix(HD,[[15,5],[16,6]])}
  if(style==='long'){row(3,9,14,H);row(4,7,16,H);row(5,6,17,H);row(6,6,17,H);for(let y=7;y<=15;y++){rect(6,y,2,1,H);rect(16,y,2,1,HS)}
    row(6,10,13,HS);pix(HL,[[9,4],[10,4],[11,4],[8,5],[9,5],[6,9],[6,10],[6,11]]);pix(HS,[[14,4],[15,4],[16,5],[17,5]])}
  if(style==='bald'){row(4,9,14,skin);pix(SK2,[[14,4],[15,5],[16,6]]);pix(light(skin,.55),[[9,4],[10,4],[8,5],[9,5],[7,6],[8,6]])}
  if(style==='curly'){row(2,9,14,H);row(3,7,16,H);row(4,5,18,H);row(5,4,19,H);row(6,4,19,H);for(let y=7;y<=10;y++){rect(4,y,3,1,H);rect(17,y,3,1,H)}rect(7,7,1,1,H);rect(16,7,1,1,H);
    for(let y=2;y<=10;y++)for(let x=4;x<=19;x++)if(g[y][x]===H){const k=(x*2+y*3)%7;if(k===0)set(x,y,HS);else if(k===3)set(x,y,HL)}
    row(6,8,15,HS);pix(HD,[[4,10],[19,10]])}
  if(style==='bun'){row(3,9,14,H);row(4,7,16,H);row(5,6,17,H);row(6,6,17,H);rect(6,7,2,2,H);rect(16,7,2,2,HS);row(6,10,13,HS);
    row(3,9,14,H);rect(10,0,4,2,H);pix('#ff8fb1',[[11,2],[12,2]]);pix(HL,[[10,0],[10,1],[9,4],[10,4],[8,5]]);pix(HS,[[13,0],[13,1],[15,4],[16,5]])}
  // sobrancelhas, olhos abertos 3x3 com brilho no mesmo canto, nariz, bochechas, sorriso
  const bc=style==='bald'?HS:HS;for(const x0 of [8,13])row(8,x0,x0+2,bc);
  const c2=shade(eyec,.62);
  for(const x0 of [8,13]){rect(x0,9,3,3,eyec);row(11,x0,x0+2,c2);set(x0+1,10,DK);set(x0,9,WH)}
  pix(SK3,[[12,12],[12,13]]);
  const bl=mix(skin,'#ff6f81',.38);pix(bl,[[7,13],[8,13],[15,13],[16,13]]);
  const mc='#a24f55';pix(mc,[[10,14],[13,14],[11,15],[12,15]]);
  // acessórios do cargo
  const A={
    C:()=>{row(5,7,16,GOLD);row(6,7,16,GOLD2);pix(GOLD,[[8,4],[9,4],[8,3],[11,4],[12,4],[11,3],[12,3],[11,2],[12,2],[14,4],[15,4],[15,3]]);
      pix('#ff5a5f',[[11,5],[12,5]]);pix('#5b9cff',[[8,5],[15,5]]);pix('#7ee0a0',[[9,5],[14,5]]);pix(GOLDL,[[7,5],[8,3],[11,2]]);pix(GOLD2,[[9,4],[12,4],[15,4]])},
    BD:()=>{row(2,9,14,GOLD);row(3,7,16,GOLD);row(4,6,17,GOLD);row(5,6,17,GOLD);row(6,5,18,GOLD2);rect(11,2,2,4,'#f7d56a');pix(GOLDL,[[9,3],[8,4],[7,5],[6,5]]);pix('#a97716',[[17,5],[16,4],[15,3],[18,6]])},
    P:()=>{const F='#44546f',FL='#8fa3c2',L='#eaf7fb';
      for(const x0 of [7,12]){rect(x0+1,9,3,3,L);row(8,x0,x0+4,F);row(12,x0,x0+4,F);for(const y of [9,10,11]){set(x0,y,F);set(x0+4,y,F)};pix(FL,[[x0+1,8],[x0+2,8]])}
      pix(F,[[11,10],[12,10],[6,10],[17,10]]);
      for(const x0 of [8,14]){rect(x0,10,2,2,eyec);set(x0,10,WH)}},
    UI:()=>{row(2,13,14,'#d9688c');row(3,9,15,'#ff8fb1');row(4,6,17,'#ff8fb1');row(5,5,17,'#ff8fb1');row(6,5,16,'#d9688c');pix('#ffc2d4',[[8,4],[9,4],[10,3],[7,5]]);pix('#c25577',[[15,5],[16,5],[17,4]])},
    RT:()=>{row(3,9,14,'#ff5a5f');row(4,7,16,'#ff5a5f');row(5,6,17,'#ff5a5f');row(6,6,17,'#ff5a5f');row(7,6,17,'#d8393f');row(2,11,12,'#d8393f');
      pix('#ff9aa0',[[9,4],[10,4],[8,5],[7,5]]);pix('#b72d33',[[15,5],[16,5],[17,6]]);},
    FX:()=>{const G='#c3ccd6',G2='#7c8794';pix(G,[[20,10],[20,11],[20,12],[23,10],[23,11],[23,12],[21,12],[22,12]]);rect(21,13,2,11,G);rect(22,13,1,11,G2);pix(G2,[[21,12],[23,10],[23,11]]);pix('#eef2f6',[[21,14],[21,15],[21,16]])},
    IN:()=>{pix(GOLD,[[8,19],[9,20],[10,21],[11,22],[15,19],[14,20],[13,21],[12,22]]);pix(GOLD2,[[8,20],[9,21],[15,20],[14,21]]);rect(10,22,4,2,'#ffffff');pix('#5b9cff',[[11,22],[12,22]]);pix('#9aa4af',[[10,23],[11,23],[12,23],[13,23]]);pix(GOLD,[[10,22]])},
    PL:()=>{const W='#e8ebef',W2='#aab3bd',W3='#7c8794';row(2,9,14,W);row(3,7,16,W);pix(W,[[6,4],[7,4],[16,4],[17,4],[5,5],[18,5],[5,6],[18,6],[4,7],[19,7],[4,8],[19,8]]);
      rect(3,9,3,5,W2);rect(18,9,3,5,W2);pix(W,[[3,9],[4,9],[5,9],[18,9],[19,9],[20,9]]);pix(W3,[[3,13],[4,13],[5,13],[18,13],[19,13],[20,13]]);
      pix(W,[[19,14],[19,15],[18,16],[17,16],[16,16]]);pix('#14161b',[[14,16],[15,16],[15,15],[14,15]])},
    DS:()=>{const W='#b38bff',W2='#7d52d6',W3='#5a36b0';row(2,9,14,W);row(3,7,16,W);pix(W,[[6,4],[7,4],[16,4],[17,4],[5,5],[18,5],[5,6],[18,6],[4,7],[19,7],[4,8],[19,8]]);
      rect(3,9,3,5,W2);rect(18,9,3,5,W2);pix(W,[[3,9],[4,9],[5,9],[18,9],[19,9],[20,9]]);pix(W3,[[3,13],[4,13],[5,13],[18,13],[19,13],[20,13]])},
    OS:()=>{pix('#aab3bd',[[11,2],[12,2]]);pix('#5b9cff',[[11,0],[12,0],[11,1],[12,1]]);pix('#bcd7ff',[[11,0]]);pix('#2f6fd6',[[12,1]])},
    OR:()=>{const X='#d5dae0',O=SH3;const G=['.X.X.','XXXXX','XXOXX','XXXXX','.X.X.'];G.forEach((r,j)=>[...r].forEach((ch,i)=>{if(ch==='X')set(4+i,19+j,X);if(ch==='O')set(4+i,19+j,O)}));pix('#9aa4af',[[8,20],[8,22],[4,22],[6,19]])},
    AR:()=>{pix('#ff8fb1',[[21,6],[21,7]]);pix(GOLD,[[20,8],[20,9],[19,10]]);pix(GOLD2,[[21,8],[21,9],[20,10]]);pix('#6d7580',[[19,11]]);pix('#14161b',[[19,12]])},
    RV:()=>{rect(17,16,6,8,'#b98a50');rect(18,18,4,5,'#ffffff');rect(18,16,4,2,'#aab3bd');pix('#7c8794',[[19,16],[20,16]]);pix('#9aa4af',[[19,19],[20,19],[21,19],[19,21],[20,21],[21,21]]);pix('#8a5f2c',[[17,20],[17,21],[17,22],[22,20],[22,21],[22,22]])},
    QA:()=>{rect(17,16,6,8,'#b98a50');rect(18,18,4,5,'#ffffff');rect(18,16,4,2,'#aab3bd');pix('#7c8794',[[19,16],[20,16]]);pix('#4cc38a',[[18,21],[19,22],[20,21],[21,20],[21,19]]);pix('#8a5f2c',[[17,20],[17,21],[17,22],[22,20],[22,21],[22,22]])},
    AG:()=>{}};
  (A[code]||A.AG)();
  // contorno de 1 px ao redor do personagem
  const out=g.map(r=>r.slice());
  for(let y=0;y<N;y++)for(let x=0;x<N;x++)if(!g[y][x]&&[[1,0],[-1,0],[0,1],[0,-1]].some(([dx,dy])=>g[y+dy]&&g[y+dy][x+dx]))out[y][x]=OUT;
  const TINT={C:'#f5e0a3',OR:'#cfd6df',BD:'#f9d9a0',P:'#cdbdf5',IN:'#bfe9d3',FX:'#d3d8de',PL:'#b9ece4',DS:'#dccbf8',UI:'#f9c9da',RT:'#f9c3c3',OS:'#bcd7f8',AR:'#f6dcb0',QA:'#c9e8c9',RV:'#c9e8c9',AG:'#dfe3e8'};
  const bg=TINT[code]||TINT.AG;
  let px=`<rect width="24" height="24" rx="4" fill="${bg}"/><rect y="17" width="24" height="7" fill="${shade(bg,.93)}"/>`;
  out.forEach((r,y)=>r.forEach((c,x)=>{if(c)px+=`<rect x="${x}" y="${y}" width="1" height="1" fill="${c}"/>`}));
  return `<svg viewBox="0 0 24 24" width="${size||48}" height="${size||48}" shape-rendering="crispEdges" style="image-rendering:pixelated;border-radius:8px">${px}</svg>`}
function showGallery(){
  document.body.classList.add('gal');
  const codes=[['C','Coordenador'],['OR','Orquestrador'],['BD','Builder'],['P','Principal'],['IN','Integrador'],['FX','Corretor'],['PL','Plugins'],['DS','DSP'],['UI','Interface'],['RT','Tempo real'],['OS','Distribuição'],['AR','Arquiteto'],['QA','Testador'],['AG','Geral']];
  const only=(location.search.match(/only=([A-Z,]+)/)||[])[1],Z=location.search.includes('huge')?144:location.search.includes('big')?96:48;
  const styles=['short','long','bald','curly','bun','side'];
  let html=only?'':'<h2 class="h2">Cabelo × pele: cada estilo com os 6 tons de pele</h2>'+(only?[]:styles).map(st=>`<div class="gal-row"><b>${st}</b>${SKINS.map((sk,i)=>{FORCE={style:st,skin:sk,hair:HAIRS[(i*3+styles.indexOf(st))%HAIRS.length]};const r=`<span class="pa">${pixelAvatar('AG',i%2===1,Z,'x'+st+i)}</span>`;FORCE=null;return r}).join('')}</div>`).join('');
  html+='<h2 class="h2">Cargos: 6 agentes diferentes de cada, Sonnet (verde-azulado) e Opus (roxo)</h2>'+['sonnet','opus'].map(m=>codes.filter(c=>!only||only.split(',').includes(c[0])).map(([c,n])=>`<div class="gal-row"><b>${n}</b>${[1,2,3,4,5,6].map(i=>`<span class="pa">${pixelAvatar(c,m==='opus',Z,'integrator-'+i+c+m[0])}</span>`).join('')}<small>${m}</small></div>`).join('')).join('');
  $('gal').innerHTML=html}
if(location.search.includes('avatars'))showGallery();
