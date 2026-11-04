// Web UI core (RS-07 section 9, FS-00 section 5): helpers h(), api(), state S, connection,
// router, page and widget registry (Shunt.registerPage / registerWidget / slot), stage mode.
// The build (cmake/build_ui.cmake) emits each ui/js file as its own <script> in name order;
// page and feature modules only call Shunt.* at load time, boot runs on DOMContentLoaded.
// Original Shunt code.
const $=s=>document.querySelector(s);
function h(tag,attrs,...kids){
  const e=document.createElement(tag);
  for(const k in (attrs||{})){
    const v=attrs[k];
    if(v==null||v===false)continue;
    if(k==='class')e.className=v; else if(k.startsWith('on'))e.addEventListener(k.slice(2),v);
    else if(k==='text')e.textContent=v; else e.setAttribute(k,v===true?'':v);
  }
  for(const c of kids.flat())if(c!=null&&c!==false)e.append(c.nodeType?c:document.createTextNode(c));
  return e;
}
const api=async(path,body)=>{
  const r=await fetch(path,body===undefined?{}:{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json().catch(()=>({}));
  if(!r.ok)throw new Error(j.error||('HTTP '+r.status));
  return j;
};
let S=null, lastMsg=0, ws=null, backoff=500, page='live';
const fmtBpm=v=>v>0?v.toFixed(1):'---.-';
const fmtTime=ms=>ms?new Date(ms).toLocaleTimeString([], {hour12:false}):'';
const pages={};

/* ---------- connection ---------- */
function connect(){
  const url=(location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws';
  ws=new WebSocket(url);
  ws.onopen=()=>{backoff=500};
  ws.onmessage=e=>{try{S=JSON.parse(e.data);lastMsg=performance.now();render()}catch(_){}};
  ws.onclose=()=>{ws=null;setTimeout(connect,backoff);backoff=Math.min(backoff*1.7,8000)};
  ws.onerror=()=>ws&&ws.close();
}
setInterval(()=>{
  const off=!S||performance.now()-lastMsg>2500;
  document.body.classList.toggle('offline',off);
  $('#offlineBanner').classList.toggle('show',off);
  $('#connDot').className='dot '+(off?'bad':(S.state==='locked'?'on':'warn'));
  $('#connTxt').textContent=off?'Offline':(S.state==='locked'?'Locked':S.state==='idle'?'No master':'Connected');
},500);

/* ---------- page and widget registry (FS-00 section 5) ---------- */
let featureList=null;           // from /api/features: [{id, enabled}]
const widgets=[];               // registered widgets
let mounted=[];                 // widgets mounted on the current page: {w, el}
const pageVisible=p=>!p.featureId||featureOn(p.featureId);
function featureOn(id){
  if(S&&S.features&&S.features[id])return !!S.features[id].enabled;
  return !!(featureList&&featureList.some(f=>f.id===id&&f.enabled));
}
function safe(what,fn){try{return fn()}catch(e){console.error('Shunt '+what+':',e)}}
const Shunt={
  // {id, title, order, featureId?, mount(root), update(status)}. id must be the first key (build checks uniqueness).
  registerPage(p){
    if(!p||!/^[a-z0-9-]+$/.test(p.id))throw new Error('registerPage: bad id');
    if(pages[p.id])throw new Error('registerPage: duplicate id '+p.id);
    if(p.order==null)p.order=100;
    pages[p.id]=p;if(navBuilt)buildNav();return p;
  },
  // {slot:'live-valve'|'live-controls'|'outputs-midi'|'outputs-osc'|'banner', id?, featureId?, order?, mount(el), update(status)}
  registerWidget(w){
    if(!w||!w.slot||typeof w.mount!=='function')throw new Error('registerWidget: need slot and mount');
    if(w.order==null)w.order=100;
    widgets.push(w);widgets.sort((a,b)=>a.order-b.order);
    if(w.slot==='banner'&&navBuilt)mountSlot($('#slot-banner'),'banner');
    return w;
  },
  // Called once by a page in mount(): returns a box-less container holding the slot's widgets.
  slot(name){const el=h('div',{class:'slot','data-slot':name});mountSlot(el,name);return el},
  h,api,$,fmtBpm,fmtTime,featureOn,
  get S(){return S},
  get pages(){return pages},
};
window.Shunt=Shunt;
function mountSlot(el,name){
  el.replaceChildren();
  mounted=mounted.filter(m=>m.slot!==el);
  for(const w of widgets.filter(w=>w.slot===name)){
    const box=h('div',{class:'w'+(w.featureId&&!featureOn(w.featureId)?' off':'')});
    el.append(box);mounted.push({w,el:box,slot:el});
    safe('widget mount',()=>w.mount(box));
  }
}
let navBuilt=false, navSig='';
function buildNav(){
  navBuilt=true;
  const list=Object.values(pages).filter(pageVisible).sort((a,b)=>a.order-b.order);
  navSig=list.map(p=>p.id).join();
  $('#nav').replaceChildren(...list.map(p=>h('a',{href:'#/'+p.id},p.title||p.id)));
  document.querySelectorAll('nav a').forEach(a=>a.setAttribute('aria-current',a.getAttribute('href')==='#/'+page?'page':'false'));
}
function refreshFeatures(){
  const sig=Object.values(pages).filter(pageVisible).sort((a,b)=>a.order-b.order).map(p=>p.id).join();
  if(sig!==navSig){buildNav();if(pages[page]&&!pageVisible(pages[page]))location.hash='#/live'}
  for(const m of mounted)if(m.w.featureId)m.el.classList.toggle('off',!featureOn(m.w.featureId));
}

/* ---------- router ---------- */
function route(){
  page=(location.hash.replace('#/','')||'live');
  if(!pages[page]||!pageVisible(pages[page]))page='live';
  document.querySelectorAll('nav a').forEach(a=>a.setAttribute('aria-current',a.getAttribute('href')==='#/'+page?'page':'false'));
  const root=$('#page');root.replaceChildren();
  mounted=mounted.filter(m=>m.w.slot==='banner');
  pages[page].mount(root);
  render();
}
function render(){
  if(S){
    const eb=$('#engineBanner');eb.textContent=S.engineError||'';eb.classList.toggle('show',!!S.engineError);
    renderFirstRun();
    $('#sbMaster').textContent=(S.masterName||'No master')+'  ·  '+fmtBpm(S.bpm)+' BPM  ·  '+(S.barKnown?S.beatInBar:'-')+'/4';
    $('#sbOut').textContent=['link','midi','osc'].map(k=>k.toUpperCase()+(S.outputs[k].connected?' ●':' ○')).join('   ');
    refreshFeatures();
  }
  if(pages[page]&&pages[page].update&&S)pages[page].update(S);
  if(S)for(const m of mounted)if(m.w.update&&!m.el.classList.contains('off'))safe('widget update',()=>m.w.update(S));
}
let ifaceList=null;
async function renderFirstRun(){
  const b=$('#firstRun');
  if(!S.firstRun||sessionStorage.getItem('frDismiss')){b.classList.remove('show');return}
  b.classList.add('show');
  if(b.dataset.built)return;b.dataset.built=1;
  ifaceList=await api('/api/interfaces');
  const sel=h('select',{'aria-label':'Network interface'},ifaceList.map(i=>h('option',{value:i.name,selected:i.name===S.interface},i.name+'  '+i.ip)));
  b.replaceChildren(h('strong',{text:'First run: '}),'Shunt is listening on ',h('b',{text:S.interface||'no interface'}),'. Choose the interface wired to the player network. If your computer also runs rekordbox, use Passive mode. Allow Shunt through the firewall for UDP 50000-50002. ',
    sel,h('button',{class:'btn primary',onclick:async()=>{await api('/api/config',{interface:sel.value});b.dataset.built='';sessionStorage.setItem('frDismiss','1');b.classList.remove('show')}},'Use this interface'),
    h('button',{class:'btn',onclick:()=>{sessionStorage.setItem('frDismiss','1');b.classList.remove('show')}},'Dismiss'));
}

/* ---------- stage mode and keys ---------- */
function stage(on){document.body.classList.toggle('stage',on);if(on){if(location.hash!=='#/live')location.hash='#/live';document.documentElement.requestFullscreen&&document.documentElement.requestFullscreen().catch(()=>{})}else if(document.fullscreenElement)document.exitFullscreen()}
$('#stageBtn').onclick=()=>stage(true);
addEventListener('keydown',e=>{
  if(e.target.matches('input,select,textarea'))return;
  if(e.key==='s'||e.key==='S')stage(!document.body.classList.contains('stage'));
  if(e.key==='Escape'&&document.body.classList.contains('stage'))stage(false);
});
addEventListener('hashchange',route);

addEventListener('DOMContentLoaded',()=>{
  buildNav();
  mountSlot($('#slot-banner'),'banner');
  route();connect();
  api('/api/features').then(f=>{if(Array.isArray(f)){featureList=f;refreshFeatures()}}).catch(()=>{});
});
