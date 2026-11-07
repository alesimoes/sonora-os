/* não reescreve o DOM quando o HTML é idêntico, e guarda o aberto/fechado de cada <details>
   (data-key ou id) entre redesenhos: a lista é refeita a cada poucos segundos */
if(typeof Element!=='undefined'){const d=Object.getOwnPropertyDescriptor(Element.prototype,'innerHTML');
  const key=(el,i)=>el.id||el.dataset.key||('i'+i);
  Object.defineProperty(Element.prototype,'innerHTML',{get(){return d.get.call(this)},set(v){
    if(this._h===v)return;this._h=v;
    const open={};this.querySelectorAll('details').forEach((x,i)=>{open[key(x,i)]=x.open});
    d.set.call(this,v);
    this.querySelectorAll('details').forEach((x,i)=>{const k=key(x,i);if(k in open)x.open=open[k]})}})}
const $=id=>document.getElementById(id);
const E=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const dur=s=>s==null?'sem sinal':s<90?s+' s':s<5400?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h';
const NAME={building:'usando a máquina',waiting_lock:'esperando a máquina',working:'trabalhando',waiting:'parado',done:'terminou',paused:'pausado',unknown:'?','no worktree':'sem worktree'};
const ORDER={working:0,building:1,waiting_lock:2,waiting:2,'no worktree':2,paused:3,done:4,unknown:5};
const PRURL='https://github.com/alesimoes/sonora-os/pull/';
let lastOk=0,lastState=null;
const until=t=>{if(!t)return '';const s=Math.max(0,Math.round(t-Date.now()/1000));const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'min':m+' min'};

const clk=t=>t?new Date(t*1000).toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit'}):'';
