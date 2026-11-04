// MIDI learn page (FS-04): enable, input port, live last-message indicator, per-action binding
// rows with Learn / Clear, countdown, inline conflict confirmation. All server text goes through
// h() / textContent. Original Shunt code.
Shunt.registerPage({id:'midilearn',title:'MIDI learn',order:22,
  mount(root){
    this.st=null;this.sig='';this.busy=false;this.lastFetch=0;this.pending=false;
    this.msg=h('div',{class:'msg','aria-live':'polite'});
    this.enable=h('input',{type:'checkbox',id:'mlEnable',onchange:()=>this.setEnabled()});
    this.portSel=h('select',{id:'mlPort','aria-label':'MIDI input port',onchange:()=>this.setPort()});
    this.conn=h('div',{class:'note ml-conn'});
    this.last=h('div',{class:'ml-last','aria-live':'off'},'No MIDI message received yet');
    this.lastDot=h('span',{class:'dot'});
    this.list=h('div',{class:'ml-list'});
    this.warn=h('div',{class:'note ml-warn'});
    root.append(
      h('div',{class:'grid2'},
        h('div',{class:'card'},h('h2',{},'Input'),
          h('label',{class:'check'},this.enable,'Enable MIDI learn'),
          h('label',{class:'field'},h('span',{},'MIDI input port'),this.portSel),
          this.conn,
          h('div',{class:'ml-lastrow'},this.lastDot,this.last),
          h('p',{class:'note'},'Shows the last note, controller or program change received, so you can see the controller working before you bind it. Clock, active sensing, SysEx, pitch bend and aftertouch are ignored.'),
          this.msg),
        h('div',{class:'card'},h('h2',{},'How it works'),
          h('p',{class:'note'},'Press Learn next to an action, then press a button, pad or footswitch within 15 seconds. A footswitch that sends CC 64 = 127 on press and 0 on release works as is. Binding a message that another action already uses asks before replacing it.'),
          this.warn)),
      h('div',{class:'card ml-actions'},h('h2',{},'Actions'),this.list));
    this.refresh(true);
  },
  async refresh(force){
    if(this.busy)return;
    const now=Date.now();
    if(!force&&now-this.lastFetch<400)return;
    this.busy=true;this.lastFetch=now;
    try{this.apply(await api('/api/f/midilearn/state'))}
    catch(e){this.say(e.message,true)}
    finally{this.busy=false}
  },
  say(t,bad){this.msg.className='msg '+(bad?'err':'ok');this.msg.textContent=t||''},
  async post(sub,body){
    try{const r=await api('/api/f/midilearn/'+sub,body||{});this.say('');await this.refresh(true);return r}
    catch(e){this.say(e.message,true);await this.refresh(true);return null}
  },
  async setEnabled(){
    const on=this.enable.checked;
    try{await api('/api/config',{features:{midilearn:{enabled:on}}});this.say(on?'Enabled.':'Disabled.')}
    catch(e){this.say(e.message,true)}
    this.refresh(true);
  },
  setPort(){this.post('port',{port:this.portSel.value})},
  apply(s){
    this.st=s;
    // Everything except the live parts decides whether the DOM is rebuilt.
    const sig=JSON.stringify([s.enabled,s.ports,s.port,s.open,s.inputError,s.learning&&s.learning.action,s.lastLearn,s.conflict,s.bindings,s.actions,s.warnings]);
    this.updateLive(s);
    if(sig===this.sig)return;
    this.sig=sig;
    this.enable.checked=!!s.enabled;
    const ports=s.ports.slice();
    if(s.port&&!ports.some(p=>p.id===s.port))ports.push({id:s.port,name:s.port+' (missing)'});
    this.portSel.replaceChildren(h('option',{value:''},'Select a port…'),...ports.map(p=>h('option',{value:p.id,selected:p.id===s.port},p.name)));
    this.portSel.disabled=false;
    this.conn.textContent=!s.enabled?'Disabled. Nothing is read from the port.':!s.port?'Choose a port to start listening.':s.open?'Listening on '+s.port:(s.inputError||'Opening…');
    this.conn.classList.toggle('bad',!!(s.enabled&&s.port&&!s.open));
    this.warn.replaceChildren(...(s.warnings||[]).map(w=>h('div',{},'Settings warning: '+w)));
    this.buildList(s);
  },
  updateLive(s){
    const m=s.lastMessage;
    if(!m){this.last.textContent='No MIDI message received yet';this.lastDot.className='dot';}
    else{
      const what=m.type==='cc'?'CC '+m.number+' = '+m.value:m.type==='program'?'Program '+m.number:'Note '+m.number+(m.on?' on, velocity '+m.value:' off');
      this.last.textContent=what+'  ·  channel '+(m.channel+1);
      this.lastDot.className='dot '+(m.ageMs<1000?'on':'');
    }
    if(this.countdown&&s.learning)this.countdown.textContent=s.learning.secondsLeft+' s';
  },
  buildList(s){
    const bound=new Map(s.bindings.map(b=>[b.action,b]));
    const known=new Set(s.actions.map(a=>a.id));
    const groups=new Map();
    for(const a of s.actions){const g=a.group||'Other';if(!groups.has(g))groups.set(g,[]);groups.get(g).push(a)}
    const kids=[];
    this.countdown=null;
    if(!s.enabled)kids.push(h('p',{class:'note'},'Enable MIDI learn above to bind actions.'));
    for(const [g,acts] of groups){
      kids.push(h('h3',{class:'ml-group'},g));
      for(const a of acts)kids.push(this.row(s,a.id,a.label,bound.get(a.id),false));
    }
    const missing=s.bindings.filter(b=>!known.has(b.action));
    if(missing.length){
      kids.push(h('h3',{class:'ml-group'},'Unknown actions'));
      for(const b of missing)kids.push(this.row(s,b.action,b.action,b,true));
    }
    this.list.replaceChildren(...kids);
    if(s.learning)this.updateLive(s);
  },
  row(s,id,label,b,missing){
    const learning=s.learning&&s.learning.action===id;
    const busy=!!s.learning&&!learning;
    const text=learning?h('span',{class:'ml-learning'},'Press a control on your device… ',this.countdown=h('b',{},s.learning.secondsLeft+' s'))
      :b?h('span',{class:'ml-bind'},b.text+' · '+b.mode+(missing?' · action not available':'')):h('span',{class:'ml-none'},'Not bound');
    const btns=[];
    if(learning)btns.push(h('button',{class:'btn on',onclick:()=>this.post('cancel')},'Cancel'));
    else{
      btns.push(h('button',{class:'btn',disabled:!s.enabled||missing||busy||null,'aria-label':'Learn '+label,onclick:()=>this.post('learn',{action:id})},b?'Re-learn':'Learn'));
      if(b)btns.push(h('button',{class:'btn',onclick:()=>this.post('unbind',{action:id}),'aria-label':'Clear binding of '+label},'Clear'));
    }
    const rows=[h('div',{class:'ml-row'+(learning?' learning':'')},h('div',{class:'ml-name'},label,h('small',{},id)),text,h('div',{class:'ml-btns'},...btns))];
    if(s.conflict&&s.conflict.action===id){
      rows.push(h('div',{class:'ml-conflict',role:'alert'},
        h('span',{},s.conflict.text+' is already bound to '+s.conflict.with.join(', ')+'. Replace it?'),
        h('button',{class:'btn primary',onclick:()=>this.post('learn',{action:id,replace:true})},'Replace'),
        h('button',{class:'btn',onclick:()=>this.post('cancel')},'Cancel')));
    }
    return h('div',{class:'ml-item'},...rows);
  },
  update(status){
    const f=status&&status.features&&status.features.midilearn;
    if(!f)return;
    if(f.learning||this.sig===''||(this.st&&(f.rev!==this.rev||!!f.learning!==!!this.st.learning||f.open!==this.st.open))){this.rev=f.rev;this.refresh(false)}
    else if(f.lastMessage)this.updateLive({lastMessage:f.lastMessage,learning:this.st&&this.st.learning});
    else if(this.st&&this.st.lastMessage)this.refresh(false);
  }
});
