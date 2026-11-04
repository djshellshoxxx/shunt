// FS-05 Replay a capture: page #/replay (captures table, upload, start options, transport) and the
// persistent amber banner in the shell `banner` slot. All file names and device names from the
// server are rendered as text (h() / textContent). Original Shunt code.
(()=>{
const RP='/api/f/replay/';
const SPEEDS=[0.25,0.5,1,2,4,8];
const mmss=s=>{s=Math.max(0,Math.floor(s||0));const h=Math.floor(s/3600),m=Math.floor(s/60)%60,x=s%60;return (h?h+':'+String(m).padStart(2,'0'):m)+':'+String(x).padStart(2,'0')};
const size=b=>b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?Math.round(b/1024)+' kB':b+' B';
const spd=v=>'x'+(v<1?String(v).replace(/^0/,''):v);
const rs=s=>(s&&s.features&&s.features.replay)||{};
const post=(p,b)=>api(RP+p,b||{});

Shunt.registerPage({id:'replay',title:'Replay',order:64,featureId:'replay',
  mount(root){
    this.msg=h('div',{class:'msg','aria-live':'polite'});
    this.body=h('tbody');
    this.speed=h('select',{'aria-label':'Start speed'},SPEEDS.map(v=>h('option',{value:v,selected:v===1},spd(v))));
    this.loop=h('input',{type:'checkbox'});
    this.outs=h('input',{type:'checkbox'});
    this.outs.addEventListener('change',()=>{if(this.outs.checked)this.speed.value='1';this.speed.disabled=this.outs.checked});
    const up=h('input',{type:'file',accept:'.pcapng',class:'sr',onchange:e=>this.upload(e.target)});
    // transport
    this.pp=h('button',{class:'btn primary rp-pp',onclick:()=>this.toggle()},'Pause');
    this.scrub=h('input',{type:'range',min:0,max:1,step:0.1,value:0,class:'rp-scrub','aria-label':'Position'});
    this.scrub.addEventListener('input',()=>{this.dragging=true;this.pos.textContent=mmss(+this.scrub.value)+' / '+mmss(this.dur)});
    this.scrub.addEventListener('change',()=>{this.dragging=false;this.cmd('seek',{t:+this.scrub.value})});
    this.pos=h('span',{class:'mono rp-pos'},'0:00 / 0:00');
    this.tspeed=h('select',{'aria-label':'Replay speed',onchange:()=>this.cmd('set',{speed:+this.tspeed.value})},SPEEDS.map(v=>h('option',{value:v},spd(v))));
    this.tloop=h('input',{type:'checkbox',onchange:()=>this.cmd('set',{loop:this.tloop.checked})});
    this.restart=h('button',{class:'btn',onclick:()=>this.cmd('seek',{t:0})},'Restart');
    this.title=h('div',{class:'rp-now'});
    this.trans=h('div',{class:'card rp-trans hidden'},h('h2',{},'Now replaying'),this.title,
      h('div',{class:'rp-bar'},this.pp,this.scrub,this.pos),
      h('div',{class:'rp-row'},h('label',{class:'rp-opt'},'Speed ',this.tspeed),h('label',{class:'rp-opt'},this.tloop,'Loop'),this.restart,
        h('button',{class:'btn danger on',onclick:()=>this.cmd('stop')},'Stop replay')));
    root.append(this.trans,
      h('div',{class:'card'},h('h2',{},'Captures'),
        h('div',{class:'rp-row'},
          h('label',{class:'rp-opt'},'Speed ',this.speed),h('label',{class:'rp-opt'},this.loop,'Loop'),
          h('label',{class:'rp-opt rp-warn'},this.outs,'Send outputs (drives MIDI, Link and OSC: off unless you want the rig to follow)'),
          h('span',{class:'rp-spacer'}),
          h('label',{class:'btn rp-upload'},'Upload .pcapng',up),
          h('button',{class:'btn',onclick:()=>this.load()},'Refresh')),
        this.msg,
        h('div',{class:'tablewrap'},h('table',{class:'rp-files'},h('thead',{},h('tr',{},['File','Size','Duration','Devices',''].map(t=>h('th',{},t)))),this.body)),
        this.dirNote=h('p',{class:'rp-note'})));
    this.load();
  },
  async load(){
    try{
      const r=await api(RP+'files');this.max=r.maxUploadBytes;
      this.dirNote.textContent='Folder: '+r.dir+'. Recordings made with the Record button land here too. While a replay runs the live network is stopped, nothing is written to the tracklist files, and outputs stay off unless "Send outputs" is ticked.';
      if(!r.files.length){this.body.replaceChildren(h('tr',{},h('td',{colspan:5,class:'rp-note'},'No captures yet. Upload a .pcapng or record one on the Diagnostics page.')));return}
      this.body.replaceChildren(...r.files.map(f=>{
        const dur=h('td',{class:'mono'},'…'),dev=h('td',{},'…');
        const fill=i=>{if(!i)return;if(i.error){dur.textContent='—';dev.textContent=i.error;dev.className='rp-err';return}
          dur.textContent=mmss(i.durationS);dev.textContent=i.devices.map(d=>d.number+' '+d.name).join(', ')||'none'};
        fill(f.info);if(!f.info)f.lazy=fill;
        return h('tr',{},h('td',{class:'rp-name'},f.name),h('td',{class:'mono'},size(f.size)),dur,dev,
          h('td',{},h('button',{class:'btn primary','aria-label':'Replay '+f.name,onclick:()=>this.start(f.name)},'Replay')));
      }));
      for(const f of r.files)if(f.lazy){try{const i=await api(RP+'info?file='+encodeURIComponent(f.name));f.lazy(i.info)}catch(_){}}
    }catch(e){this.say(e.message,true)}
  },
  say(t,bad){this.msg.className='msg'+(bad?' err':' ok');this.msg.textContent=t},
  async start(name){
    this.say('Loading '+name+'…');
    try{await post('start',{file:name,speed:+this.speed.value,loop:this.loop.checked,outputs:this.outs.checked});this.say('Replaying '+name+'.')}
    catch(e){this.say(e.message,true)}
  },
  async cmd(p,b){try{await post(p,b)}catch(e){this.say(e.message,true)}},
  toggle(){const r=rs(S);this.cmd(r.paused?'resume':'pause')},
  async upload(inp){
    const f=inp.files[0];inp.value='';if(!f)return;
    if(this.max&&f.size>this.max){this.say(f.name+' is larger than '+size(this.max)+'.',true);return}
    this.say('Uploading '+f.name+'…');
    try{
      let r=await fetch(RP+'upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
      let j=await r.json().catch(()=>({}));
      if(r.status===409&&confirm(f.name+' already exists. Replace it?')){r=await fetch(RP+'upload?replace=1&name='+encodeURIComponent(f.name),{method:'POST',body:f});j=await r.json().catch(()=>({}))}
      if(!r.ok)throw new Error(j.error||('HTTP '+r.status));
      this.say('Uploaded '+j.name+' ('+size(j.size)+').');this.load();
    }catch(e){this.say(e.message,true)}
  },
  update(s){
    const r=rs(s),on=!!r.active;
    this.trans.classList.toggle('hidden',!on);
    if(!on)return;
    this.dur=r.durationS;
    this.title.textContent=r.file+(r.ended?'  (ended)':r.paused?'  (paused)':'')+(r.outputs?'  ·  outputs ON':'');
    this.pp.textContent=r.paused?'Play':'Pause';this.pp.disabled=!!r.ended;
    this.restart.classList.toggle('hidden',!r.ended);
    this.scrub.max=String(r.durationS||1);
    if(!this.dragging){this.scrub.value=String(r.positionS);this.pos.textContent=mmss(r.positionS)+' / '+mmss(r.durationS)}
    if(document.activeElement!==this.tspeed)this.tspeed.value=String(r.speed);
    this.tspeed.disabled=!!r.outputs;
    this.tloop.checked=!!r.loop;
  }
});

// Persistent amber banner while a replay runs (shell slot between header and main).
Shunt.registerWidget({slot:'banner',featureId:'replay',order:10,
  mount(el){
    this.txt=h('span',{class:'rp-btxt'});
    this.el=h('div',{class:'rp-banner',role:'status','aria-live':'polite'},h('b',{},'REPLAY'),this.txt,
      h('a',{href:'#/replay',class:'rp-blink'},'Open'),
      h('button',{class:'btn',onclick:()=>post('stop').catch(()=>{})},'Stop'));
    el.append(this.el);
  },
  update(s){
    const r=rs(s);
    this.el.classList.toggle('show',!!r.active);
    if(!r.active)return;
    this.txt.textContent=r.file+'  '+mmss(r.positionS)+' / '+mmss(r.durationS)+'  '+spd(r.speed)+(r.paused?'  paused':'')+(r.ended?'  ended':'')+(r.outputs?'  ·  OUTPUTS ON':'  ·  outputs off');
  }
});
})();
