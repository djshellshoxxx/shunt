// Phase scope page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- phase scope ---------- */
Shunt.registerPage({id:'scope',title:'Phase scope',order:30,
  mount(root){
    this.cv=h('canvas',{id:'scope',role:'img','aria-label':'Phase residuals, last 64 beats'});
    this.stats={rms:h('b',{},'–'),med:h('b',{},'–'),lat:h('b',{},'–')};
    root.append(h('div',{class:'card'},h('h2',{},'Phase scope: last 64 beats'),this.cv,
      h('div',{class:'stats'},h('div',{class:'stat'},this.stats.rms,h('span',{},'RMS ms')),h('div',{class:'stat'},this.stats.med,h('span',{},'Median ms')),h('div',{class:'stat'},this.stats.lat,h('span',{},'Latency offset ms'))),
      h('div',{style:'display:flex;gap:10px'},h('button',{class:'btn',onclick:async e=>{try{await api('/api/latency',{fromScope:true})}catch(x){e.target.textContent=x.message}}},'Centre latency on scope median')),
      h('p',{class:'note'},'Each dot is how far a master beat arrived from where the filter expected it. Spread shows network and player jitter; the guide lines are ±1 ms and ±4 ms.')));
  },
  update(s){
    const sc=s.scope;this.stats.rms.textContent=sc.rmsMs.toFixed(2);this.stats.med.textContent=sc.medianMs.toFixed(2);this.stats.lat.textContent=sc.latencyMs.toFixed(1);
    const cv=this.cv,dpr=devicePixelRatio||1,w=cv.clientWidth,hh=cv.clientHeight;
    if(cv.width!==w*dpr){cv.width=w*dpr;cv.height=hh*dpr}
    const g=cv.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,w,hh);
    const css=getComputedStyle(document.documentElement),col=n=>css.getPropertyValue(n).trim();
    const R=8,mid=hh/2,sy=v=>mid-(v/R)*(hh/2-10);
    g.font='11px system-ui';g.textBaseline='middle';
    [[0,'0'],[1,'1'],[-1,''],[4,'4 ms'],[-4,'']].forEach(([v,t])=>{
      g.strokeStyle=v===0?col('--line2'):col('--line');g.setLineDash(v===0?[]:[4,4]);g.beginPath();g.moveTo(34,sy(v));g.lineTo(w,sy(v));g.stroke();
      g.fillStyle=col('--faint');if(t)g.fillText(t,4,sy(v)-(v<0?0:0))});
    g.setLineDash([]);
    const r=sc.residualsMs;if(!r.length){g.fillStyle=col('--dim');g.fillText('Waiting for master beats…',48,mid-20);return}
    const dx=(w-50)/63;
    g.strokeStyle=col('--accent2');g.lineWidth=1.5;g.beginPath();
    r.forEach((v,i)=>{const x=40+(64-r.length+i)*dx,y=sy(Math.max(-R,Math.min(R,v)));i?g.lineTo(x,y):g.moveTo(x,y)});g.stroke();
    r.forEach((v,i)=>{const x=40+(64-r.length+i)*dx,y=sy(Math.max(-R,Math.min(R,v)));g.fillStyle=Math.abs(v)>4?col('--bad'):Math.abs(v)>1?col('--warn'):col('--accent');g.beginPath();g.arc(x,y,3,0,7);g.fill()});
  }
});
