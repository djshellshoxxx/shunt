// Outputs page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- outputs / settings ---------- */
Shunt.registerPage({id:'outputs',title:'Outputs',order:20,
  mount(root){
    this.msg=h('div',{class:'msg','aria-live':'polite'});
    this.form=h('div',{class:'grid2'});root.append(this.form,h('div',{style:'margin-top:16px;display:flex;gap:14px;align-items:center'},h('button',{class:'btn primary',id:'saveBtn',onclick:()=>this.save()},'Save and apply'),this.msg));
    Promise.all([api('/api/config'),api('/api/midi-ports'),api('/api/interfaces')]).then(([c,p,i])=>{this.cfg=c;this.build(c,p,i)});
  },
  build(c,ports,ifaces){
    const f={};this.f=f;
    const text=(k,label,v,type='text')=>{f[k]=h('input',{type,value:v==null?'':v});return h('label',{class:'field'},h('span',{},label),f[k])};
    const sel=(k,label,opts,v)=>{f[k]=h('select',{},opts.map(([val,t])=>h('option',{value:val,selected:String(val)===String(v)},t)));return h('label',{class:'field'},h('span',{},label),f[k])};
    const chk=(k,label,v)=>{f[k]=h('input',{type:'checkbox',checked:!!v});return h('label',{class:'check'},f[k],label)};
    const M=c.midi||{},O=c.osc||{};
    this.form.replaceChildren(
      h('div',{class:'card'},h('h2',{},'Network'),
        sel('interface','Interface (wired to the player network)',[['', 'Automatic'],...ifaces.map(i=>[i.name,i.name+'  '+i.ip])],c.interface),
        sel('mode','Mode',[['follow','Follow: join as a virtual device'],['passive','Passive: listen only (use with rekordbox on this computer)']],c.mode),
        text('deviceNumber','Virtual device number (1–6)',c.deviceNumber,'number'),
        text('venue','Venue (used in exports)',c.venue),text('performer','Performer (used in CUE sheets)',c.performer)),
      h('div',{class:'card'},h('h2',{},'Ableton Link'),chk('linkEnabled','Enable Link output',c.link&&c.link.enabled),
        h('p',{class:'note'},S&&!S.outputs.link.available?'This build has no Link SDK. Link needs a licensed build; MIDI clock and OSC work now.':'Quantum 4 when the bar is known, otherwise 1.')),
      h('div',{class:'card'},h('h2',{},'MIDI clock'),chk('midiEnabled','Enable MIDI clock output',M.enabled),
        sel('midiPort','Port',[['','Select a port…'],...ports.map(p=>[p.id,p.name]),...(M.port&&!ports.find(p=>p.id===M.port)?[[M.port,M.port+' (missing)']]:[])],M.port),
        sel('midiPause','When the deck pauses',[['keep','Keep clock running at last tempo'],['stop','Send Stop, Continue on resume']],M.pause),
        chk('startFirst','Send Start on first lock',M.startOnFirstLock!==false),chk('startReset','Send Start on reset',M.startOnReset),chk('spp','Send Song Position on reset',M.sppOnReset),
        h('p',{class:'note'},ports.length?'Raw MIDI devices found on this host.':'No raw MIDI devices found. On a desktop, use the JUCE shell for system MIDI ports; on the Box use the DIN UART or a USB MIDI interface.'),Shunt.slot('outputs-midi')),
      h('div',{class:'card'},h('h2',{},'OSC'),chk('oscEnabled','Enable OSC output',O.enabled),
        text('oscHost','Destination host',O.host),text('oscPort','Destination port',O.port,'number'),
        sel('oscProfile','Profile',[['generic','Generic /shunt'],['resolume','Resolume'],['grandma3','grandMA3'],['magicq','MagicQ'],['qlab','QLab']],O.profile),
        chk('oscInEnabled','Accept OSC control: /shunt/baroffset i, /shunt/beatonly i',O.inEnabled),text('oscInPort','Listen on UDP port',O.inPort,'number'),
        text('qlabMaster','QLab cue on master change',O.qlabMasterCue,'number'),text('qlabTrack','QLab cue on track load',O.qlabTrackCue,'number'),Shunt.slot('outputs-osc')));
  },
  async save(){
    const f=this.f;if(!f)return;const v=k=>f[k].type==='checkbox'?f[k].checked:f[k].value;
    const patch={interface:v('interface'),mode:v('mode'),deviceNumber:+v('deviceNumber'),venue:v('venue'),performer:v('performer'),
      link:{enabled:v('linkEnabled')},
      midi:{enabled:v('midiEnabled'),port:v('midiPort'),pause:v('midiPause'),startOnFirstLock:v('startFirst'),startOnReset:v('startReset'),sppOnReset:v('spp')},
      osc:{enabled:v('oscEnabled'),host:v('oscHost'),port:+v('oscPort'),profile:v('oscProfile'),inEnabled:v('oscInEnabled'),inPort:+v('oscInPort'),qlabMasterCue:+v('qlabMaster'),qlabTrackCue:+v('qlabTrack')}};
    try{await api('/api/config',patch);this.msg.className='msg ok';this.msg.textContent='Saved and applied.'}
    catch(e){this.msg.className='msg err';this.msg.textContent=e.message}
  },
  update(){}
});
