// Live page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- live ---------- */
Shunt.registerPage({id:'live',title:'Live',order:10,
  mount(root){
    const L=this.refs={};
    const R=44,C=2*Math.PI*R;
    L.players=h('div',{class:'col'});L.outs=h('div',{class:'col side'});
    const svg=`<svg viewBox="0 0 100 100"><circle class="track" cx="50" cy="50" r="${R}" stroke-width="3"/><circle class="val" id="confArc" cx="50" cy="50" r="${R}" stroke-width="3" stroke-dasharray="${C}" stroke-dashoffset="${C}"/></svg>`;
    const ring=h('div',{class:'ring'});ring.innerHTML=svg;
    L.bpm=h('div',{class:'bigbpm','aria-live':'off'},'---.-');L.state=h('div',{class:'statelbl'},'idle');
    L.mid=h('div',{class:'mid'},L.bpm,h('div',{class:'unit'},'BPM'),L.state);ring.append(L.mid);
    L.arc=ring.querySelector('#confArc');L.C=C;
    L.lamps=[1,2,3,4].map(i=>h('i',{class:'lamp'+(i===1?' one':'')}));
    L.barbeat=h('div',{class:'barbeat'},'-/4');L.nobar=h('div',{class:'nobar hidden'},'No bar information: beat only');
    L.conf=h('div',{class:'note'},'');L.master=h('div',{class:'note'},'');
    const valve=h('div',{class:'card valve'},h('h2',{},'Shunt'),ring,h('div',{class:'lamps'},L.lamps),L.barbeat,L.nobar,L.master,L.conf,Shunt.slot('live-valve'));
    L.players.append(h('h2',{},'Player network'));L.outs.append(h('h2',{},'Outputs'));
    L.devs=h('div',{class:'col',style:'gap:12px'});L.players.append(L.devs);
    L.outList=h('div',{class:'col',style:'gap:12px'});L.outs.append(L.outList);
    root.append(h('div',{class:'live'},L.players,valve,L.outs));
    // controls
    const seg=h('div',{class:'seg',role:'group','aria-label':'Bar offset'},[0,1,2,3].map(i=>h('button',{'aria-pressed':'false',onclick:()=>api('/api/baroffset',{value:i})},String(i))));
    L.seg=seg;
    L.beatOnly=h('button',{class:'btn','aria-pressed':'false',onclick:()=>api('/api/beatonly',{value:!S.beatOnly})},'Beat only');
    L.mode=h('div',{class:'seg',role:'group','aria-label':'Mode'},['follow','passive'].map(m=>h('button',{'data-m':m,'aria-pressed':'false',onclick:()=>api('/api/config',{mode:m})},m==='follow'?'Follow':'Passive')));
    L.lat=h('span',{class:'v'},'');
    const step=d=>()=>api('/api/latency',{ms:Math.round((S.scope.latencyMs+d)*10)/10});
    const lat=h('div',{class:'stepper'},h('button',{class:'btn','aria-label':'Latency minus',onclick:step(-0.5)},'−'),L.lat,h('button',{class:'btn','aria-label':'Latency plus',onclick:step(0.5)},'+'));
    L.prof=h('div',{class:'seg',role:'group','aria-label':'Profile'},['stage','rehearsal'].map(m=>h('button',{'data-m':m,'aria-pressed':'false',onclick:()=>api('/api/config',{profile:m})},m[0].toUpperCase()+m.slice(1))));
    L.rec=h('button',{class:'btn danger','aria-pressed':'false',onclick:()=>api('/api/record',{on:!S.recording})},'● Record');
    const menu=h('details',{class:'menu'},h('summary',{class:'btn'},'Export tracklist ▾'),h('div',{class:'pop'},
      [['csv','CSV'],['cue','CUE sheet'],['txt','Timestamp text'],['ndjson','NDJSON'],['report','Performance report CSV']].map(([f,t])=>h('a',{href:'/api/export?format='+f,download:''},t))));
    root.append(h('div',{class:'card controls'},
      h('div',{class:'ctl'},h('span',{class:'lbl'},'Bar offset'),seg),L.beatOnly,
      h('div',{class:'ctl'},h('span',{class:'lbl'},'Mode'),L.mode),
      h('div',{class:'ctl'},h('span',{class:'lbl'},'Latency'),lat),
      h('div',{class:'ctl'},h('span',{class:'lbl'},'Profile'),L.prof),
      Shunt.slot('live-controls'),
      h('div',{style:'margin-left:auto;display:flex;gap:10px'},L.rec,menu)));
    // tracklist strip
    L.strip=h('tbody');
    root.append(h('div',{class:'card strip'},h('h2',{},'Tracklist'),h('div',{class:'tablewrap'},h('table',{},h('thead',{},h('tr',{},['Time','Deck','Track','Played'].map(t=>h('th',{},t)))),L.strip))));
    this.rows=-1;
  },
  update(s){
    const L=this.refs;
    L.bpm.textContent=fmtBpm(s.bpm);
    L.state.textContent=s.state;L.state.className='statelbl '+s.state;
    const c=Math.max(0,Math.min(1,s.confidence));
    L.arc.style.strokeDashoffset=L.C*(1-c);
    L.arc.style.stroke=c>.8?'var(--accent)':c>.5?'var(--warn)':'var(--bad)';
    L.lamps.forEach((l,i)=>l.classList.toggle('on',s.playing&&s.barKnown?(i+1)===s.beatInBar:(!s.barKnown&&s.playing&&i===((s.beatIndex%4)+4)%4)));
    L.barbeat.textContent=s.barKnown?s.beatInBar+' / 4':'– / –';
    L.nobar.classList.toggle('hidden',s.barKnown||s.state==='idle');
    L.master.textContent=s.masterName?('Master: '+s.masterName+' (#'+s.master+')'):'Waiting for a master deck';
    L.conf.textContent='Confidence '+Math.round(c*100)+'%';
    [...L.seg.children].forEach((b,i)=>b.setAttribute('aria-pressed',String(i===s.barOffset)));
    L.beatOnly.classList.toggle('on',s.beatOnly);L.beatOnly.setAttribute('aria-pressed',String(s.beatOnly));
    [...L.mode.children].forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.m===s.mode)));
    [...L.prof.children].forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.m===s.profile)));
    L.lat.textContent=s.scope.latencyMs.toFixed(1)+' ms';
    L.rec.classList.toggle('on',s.recording);L.rec.setAttribute('aria-pressed',String(s.recording));
    L.rec.textContent=s.recording?'● Recording':'● Record';
    // players
    const sig=JSON.stringify(s.devices.map(d=>[d.number,d.name,d.model,d.master,d.playing,d.onAir,Math.round((d.bpm||0)*10),d.trackId]));
    if(sig!==this.sig){
      this.sig=sig;
      if(!s.devices.length)L.devs.replaceChildren(h('div',{class:'empty'},'No players found yet. Connect Shunt to the player network switch, or check the interface in Outputs → Network.'));
      else L.devs.replaceChildren(...s.devices.map(d=>h('div',{class:'tile'+(d.master?' master':'')+(d.onAir?' onair':'')},
        h('div',{class:'num'},String(d.number)),
        h('div',{class:'name'},d.master?h('span',{class:'crown',title:'Tempo master'},'♛'):null,d.name),
        d.kind==='mixer'||d.bpm==null?h('div',{class:'bpm'},''):h('div',{class:'bpm'},fmtBpm(d.bpm),h('small',{},(d.pitchPct>=0?'+':'')+(d.pitchPct||0).toFixed(1)+'%')),
        h('div',{class:'sub'},h('a',{class:'badge '+d.badge,href:'#/compat',title:'Compatibility'},d.model),
          d.kind==='mixer'?null:h('span',{},d.playing?'▶ playing':'■ stopped'),d.trackId?h('span',{class:'mono'},'ID '+d.trackId):null,d.onAir?h('b',{style:'color:var(--bad)'},'ON AIR'):null)))); }
    // outputs
    const defs=[['link','LINK',o=>o.available?(o.peers+' peers'):'SDK not built in'],['midi','MIDI',o=>o.detail||'no port'],['osc','OSC',o=>o.detail]];
    L.outList.replaceChildren(...defs.map(([k,t,f])=>{const o=s.outputs[k];
      return h('a',{class:'tile out',href:'#/outputs',style:'text-decoration:none;color:inherit'},h('div',{class:'num'},t),
        h('div',{},h('div',{class:'name'},h('i',{class:'dot '+(o.connected?'on':o.enabled?'bad':'')}),' ',o.enabled?(o.connected?'Active':'Problem'):'Off'),
          h('div',{class:'sub'},f(o),k==='midi'&&o.enabled&&o.connected?h('span',{class:'mono'},'jitter '+o.jitterUs.toFixed(0)+' µs'):null,k!=='link'&&o.enabled?h('span',{class:'mono'},o.sent+' msgs'):null)),
        o.error&&o.enabled?h('div',{class:'err'},o.error):null)}));
    if(s.tracks!==this.rows){this.rows=s.tracks;api('/api/tracklist').then(t=>{
      const rows=t.rows.slice(-5).reverse();
      L.strip.replaceChildren(...(rows.length?rows.map(r=>h('tr',{},h('td',{class:'mono'},fmtTime(r.startedAt)),h('td',{},'#'+r.deck),h('td',{},r.title||r.artist?((r.artist||'ID')+' - '+(r.title||'ID')):('ID '+r.rekordboxId)),h('td',{},Math.round(r.playedPct)+'%'))):[h('tr',{},h('td',{colspan:4,class:'note'},'Nothing logged yet. A track counts after 30 s on air.'))]))})}
  }
});
