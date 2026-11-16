// FS-06 Nudge pad: page #/nudge. Giant TAP button (fires on pointerdown) that sets the downbeat,
// -1 / -0.1 / +0.1 / +1 ms latency buttons with hold-to-repeat, Undo, confidence bars and a
// rolling change log. Server text is always rendered through h() / textContent. Wake Lock and
// vibrate are feature-detected and silently skipped. Original Shunt code.
(()=>{
const NP='/api/f/nudge/';
const ns=()=>(S&&S.features&&S.features.nudge)||{};
const r1=v=>Math.round(v*10)/10;
const fmtMs=v=>(v>=0?'+':'')+v.toFixed(1);
const STEPS=[['−1',-1],['−0.1',-0.1],['+0.1',0.1],['+1',1]];

Shunt.registerPage({id:'nudge',title:'Nudge',order:65,featureId:'nudge',
  mount(root){
    const P=this.refs={};
    this.chain=Promise.resolve();this.lat=null;this.latAt=0;this.toastT=0;this.logSig='';
    P.bpm=h('div',{class:'nd-bpm mono'},'---.-');
    P.lamps=[1,2,3,4].map(i=>h('i',{class:'lamp'+(i===1?' one':'')}));
    P.state=h('div',{class:'nd-state'},'idle');
    const top=h('section',{class:'nd-top card','aria-label':'Valve'},P.bpm,h('div',{class:'lamps'},P.lamps),P.state);
    P.tap=h('button',{class:'nd-tap',type:'button','aria-label':'Tap on the downbeat'},h('span',{class:'nd-tapt'},'TAP'),h('span',{class:'nd-tapsub'},'on the one'));
    P.tap.addEventListener('pointerdown',e=>{e.preventDefault();this.tap()});
    P.tap.addEventListener('keydown',e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();if(!e.repeat)this.tap()}});
    P.why=h('div',{class:'nd-why'},'');
    P.toast=h('div',{class:'nd-toast','aria-live':'polite',role:'status'},'');
    const mid=h('section',{class:'nd-mid'},P.tap,P.why);
    P.btns=STEPS.map(([t,d])=>this.holdBtn(t,d));
    P.undo=h('button',{class:'btn nd-undo',type:'button',onclick:()=>this.undo()},'Undo');
    P.lat=h('div',{class:'nd-lat mono'},'Latency --');
    P.reset=h('button',{class:'btn nd-reset',type:'button',onclick:()=>this.post('reset',{})},'Reset');
    P.bars=[['hit','Hit rate','50%'],['jit','Jitter','30%'],['lock','Locked','20%']].map(([k,t,w])=>{
      const fill=h('i');const sent=h('span',{class:'nd-sent'},'');
      return {k,fill,sent,el:h('div',{class:'nd-bar'},h('div',{class:'nd-barh'},h('b',{},t),h('small',{},'weight '+w)),h('div',{class:'nd-track'},fill),sent)};
    });
    P.formula=h('div',{class:'nd-formula'},'');
    P.log=h('ol',{class:'nd-log','aria-label':'Recent latency changes'});
    const bottom=h('section',{class:'nd-bot'},
      h('div',{class:'nd-row'},P.btns),
      h('div',{class:'nd-row nd-row2'},P.lat,P.reset,P.undo),
      P.toast,
      h('div',{class:'card nd-conf'},h('h2',{},'Why this confidence'),P.bars.map(b=>b.el),P.formula),
      h('details',{class:'nd-logbox'},h('summary',{},'Recent changes'),P.log));
    root.append(h('div',{class:'nd'},top,mid,bottom));
    this.wake();
  },
  update(s){
    const P=this.refs,f=ns();if(!P)return;
    P.bpm.textContent=fmtBpm(s.bpm);
    P.lamps.forEach((l,i)=>l.classList.toggle('on',!!(s.playing&&s.barKnown&&(i+1)===s.beatInBar)));
    P.state.textContent=s.state+(s.barKnown?'':' · no bar info');
    const ready=!!f.tapReady;
    P.tap.classList.toggle('blocked',!ready);
    P.why.textContent=ready?'':(f.tapBlocked||'');
    P.undo.disabled=!f.canUndo;
    if(typeof f.latencyMs==='number'&&Date.now()-this.latAt>600)this.lat=f.latencyMs;
    P.lat.textContent='Latency '+(this.lat==null?'--':this.lat.toFixed(1))+' ms · bar offset '+(f.barOffset==null?'-':f.barOffset);
    if(f.hitRate!=null){
      const hr=f.hitRate,js=f.jitterScore||0,lk=f.locked?1:0;
      const set=(b,v,t)=>{b.fill.style.width=Math.round(Math.max(0,Math.min(1,v))*100)+'%';b.fill.className=v>.8?'ok':v>.5?'mid':'bad';b.sent.textContent=t};
      set(P.bars[0],hr,f.hitsOf?f.hits+' of '+f.hitsOf+' beats arrived on time':'waiting for beats');
      set(P.bars[1],js,'jitter '+(f.rmsMs||0).toFixed(1)+' ms');
      set(P.bars[2],lk,f.locked?'locked':'not locked ('+f.state+')');
      P.formula.textContent='Confidence '+Math.round((f.confidence||0)*100)+'% = '+(f.formula||'');
    }
    const log=f.log||[],sig=JSON.stringify(log);
    if(sig!==this.logSig){
      this.logSig=sig;
      P.log.replaceChildren(...(log.length?log.map(e=>h('li',{class:'mono'},fmtTime(e.t)+'  '+e.from.toFixed(1)+' → '+e.to.toFixed(1)+' ms')):[h('li',{class:'nd-none'},'No changes yet')]));
    }
    if(location.hash!=='#/nudge')this.release();
  },
  say(t,bad){
    const el=this.refs.toast;el.textContent=t;el.className='nd-toast '+(bad?'err':'ok');
    clearTimeout(this.toastT);this.toastT=setTimeout(()=>{el.textContent='';el.className='nd-toast'},4000);
  },
  buzz(){if(ns().haptics!==false&&navigator.vibrate)try{navigator.vibrate(15)}catch(_){}},
  async post(p,b){
    try{const r=await api(NP+p,b);if(typeof r.latencyMs==='number'){this.lat=r.latencyMs;this.latAt=Date.now()}return r}
    catch(e){this.say(e.message,true);return null}
  },
  async tap(){
    this.buzz();
    try{
      const r=await api(NP+'tap',{});
      this.say('Downbeat set: bar offset '+r.offset+' (was '+r.previous+')',false);
    }catch(e){if(!/too fast/.test(e.message))this.say(e.message,true)}
  },
  async undo(){
    try{const r=await api(NP+'undo',{});this.say('Undone: bar offset '+r.offset,false)}
    catch(e){this.say(e.message,true)}
  },
  // Steps are sent one after another so hold-repeat can never reorder them.
  step(d){
    this.chain=this.chain.then(async()=>{
      const f=ns(),cur=this.lat!=null?this.lat:(S&&S.scope?S.scope.latencyMs:0);
      const coarse=Math.abs(Math.round(d/0.5)*0.5-d)<1e-9;
      try{
        if(f.fine||coarse){
          const r=await api(NP+'step',{ms:d});
          if(typeof r.latencyMs==='number'){this.lat=r.latencyMs;this.latAt=Date.now()}
        }else{                       // no exact server-side set: use the regular latency route like the Live page
          const t=Math.max(-20,Math.min(50,r1(cur+d)));
          await api('/api/latency',{ms:t});this.lat=t;this.latAt=Date.now();
        }
      }catch(e){this.say(e.message,true)}
    });
  },
  holdBtn(label,base){
    let timer=0,iv=0,n=0;
    const sign=base<0?-1:1;
    const stop=()=>{clearTimeout(timer);clearInterval(iv);timer=iv=0;n=0};
    const b=h('button',{class:'btn nd-step',type:'button','aria-label':'Latency '+label+' ms'},label);
    b.addEventListener('pointerdown',e=>{
      e.preventDefault();stop();
      try{b.setPointerCapture(e.pointerId)}catch(_){}
      this.step(base);
      timer=setTimeout(()=>{iv=setInterval(()=>{n++;this.step(sign*(n<=5?0.1:n<=10?0.5:1))},120)},400);
    });
    for(const ev of ['pointerup','pointercancel','lostpointercapture','pointerleave'])b.addEventListener(ev,stop);
    b.addEventListener('keydown',e=>{if((e.key==='Enter'||e.key===' ')&&!e.repeat){e.preventDefault();this.step(base)}});
    return b;
  },
  async wake(){
    if(ns().wakeLock===false||!navigator.wakeLock||this.lock)return;
    try{this.lock=await navigator.wakeLock.request('screen');this.lock.addEventListener('release',()=>{this.lock=null})}catch(_){this.lock=null}
  },
  release(){if(this.lock){try{this.lock.release()}catch(_){}this.lock=null}},
});

// Keyboard: Space = tap, arrows = nudge (left/right 0.1 ms, up/down 1 ms). Only on this page.
addEventListener('keydown',e=>{
  if(location.hash!=='#/nudge'||e.target.matches('input,select,textarea,button')||e.ctrlKey||e.metaKey||e.altKey)return;
  const pg=Shunt.pages.nudge;if(!pg||!pg.refs)return;
  const map={ArrowLeft:-0.1,ArrowRight:0.1,ArrowDown:-1,ArrowUp:1};
  if(e.key===' '||e.code==='Space'){e.preventDefault();if(!e.repeat)pg.tap()}
  else if(map[e.key]!=null){e.preventDefault();pg.step(map[e.key])}
});
document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible'&&location.hash==='#/nudge'){const pg=Shunt.pages.nudge;pg&&pg.refs&&pg.wake()}});
})();
