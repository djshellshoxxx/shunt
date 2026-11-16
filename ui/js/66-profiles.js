// FS-07 Venue profiles: page #/profiles (list, save current, inline diff + confirm, rename, duplicate,
// delete, export, import) and a quick-switch widget in the Live control bar. Every name and value from
// the server is rendered as text (h() / textContent). Original Shunt code.
(()=>{
const P='/api/f/profiles/';
const ps=s=>(s&&s.features&&s.features.profiles)||{};
const post=(p,b)=>api(P+p,b||{});
const PR={list:[],active:'',activeName:'',dirty:false,rev:-1,backend:'',pending:null,watchers:[]};
const val=v=>v===undefined||v===null?'(unset)':typeof v==='string'?(v===''?'""':v):JSON.stringify(v);

async function reload(){
  const r=await api(P+'list');
  PR.list=r.profiles||[];PR.active=r.active||'';PR.activeName=r.activeName||'';PR.dirty=!!r.dirty;PR.backend=r.backend||'';
  for(const w of PR.watchers)w();
  return r;
}
// Re-fetch the list whenever the server says the list or the active profile changed.
function track(s){
  const f=ps(s);
  if(f.rev!=null&&f.rev!==PR.rev){PR.rev=f.rev;reload().catch(()=>{})}
  else if(f.dirty!=null&&(f.dirty!==PR.dirty||f.active!==PR.active)){PR.dirty=!!f.dirty;PR.active=f.active||'';for(const w of PR.watchers)w()}
}

Shunt.registerPage({id:'profiles',title:'Profiles',order:66,featureId:'profiles',
  mount(root){
    this.msg=h('div',{class:'msg','aria-live':'polite'});
    this.rows=h('div',{class:'pf-list'});
    this.name=h('input',{type:'text',maxlength:80,placeholder:'Club A (wired)','aria-label':'Profile name',class:'pf-name',
      onkeydown:e=>{if(e.key==='Enter')this.save(false)}});
    this.inc=h('input',{type:'checkbox'});
    this.over=h('div',{class:'pf-over hidden'});
    const file=h('input',{type:'file',accept:'.json,application/json',class:'sr',onchange:e=>this.import(e.target)});
    this.warn=h('p',{class:'pf-warn hidden'});
    root.append(
      h('div',{class:'card'},h('h2',{},'Save current settings as a profile'),
        h('div',{class:'pf-row'},this.name,
          h('label',{class:'pf-opt'},this.inc,'Include network interface'),
          h('button',{class:'btn primary',onclick:()=>this.save(false)},'Save current as…')),
        this.over,
        h('p',{class:'note'},'A profile stores outputs, MIDI, OSC, latency, mode, venue and every feature’s settings. Bar offsets and the web server address are never included. The interface is saved but only applied when ticked here.')),
      h('div',{class:'card',style:'margin-top:20px'},h('h2',{},'Profiles'),this.warn,this.rows,this.msg,
        h('div',{class:'pf-row'},h('label',{class:'btn pf-import'},'Import…',file),
          h('button',{class:'btn',onclick:()=>this.load()},'Refresh'))));
    this.open=null;this.diffs={};
    this.watch=()=>this.draw();PR.watchers.push(this.watch);
    this.load().then(()=>{if(PR.pending){const s=PR.pending;PR.pending=null;this.showDiff(s)}});
  },
  update(s){track(s);const f=ps(s);this.warn.textContent=f.warning||'';this.warn.classList.toggle('hidden',!f.warning)},
  say(t,bad){this.msg.className='msg'+(bad?' err':' ok');this.msg.textContent=t},
  async load(){try{await reload();this.draw()}catch(e){this.say(e.message,true)}},
  draw(){
    if(!this.rows||!this.rows.isConnected)return;
    if(!PR.list.length){this.rows.replaceChildren(h('div',{class:'empty'},'No profiles yet. Set the rig up, then save it above.'));return}
    this.rows.replaceChildren(...PR.list.map(p=>this.row(p)));
  },
  row(p){
    const act=p.slug===PR.active;
    const mk=(t,fn,cls)=>h('button',{class:'btn '+(cls||''),onclick:fn,'aria-label':t+' '+p.name},t);
    const item=h('div',{class:'pf-item'+(act?' active':'')},
      h('div',{class:'pf-head'},
        h('div',{class:'pf-title'},h('span',{class:'pf-nm'},p.name),
          act?h('span',{class:'badge full'},'Active'):null,
          act&&PR.dirty?h('span',{class:'pf-dirty',title:'Settings differ from this profile','aria-label':'Modified since applied'},'● modified'):null,
          h('small',{class:'mono'},p.slug)),
        h('div',{class:'pf-btns'},mk('Apply',()=>this.showDiff(p.slug),'primary'),mk('Rename',()=>this.rename(p)),mk('Duplicate',()=>this.dup(p)),
          h('a',{class:'btn',href:P+'export?slug='+encodeURIComponent(p.slug),download:p.slug+'.json','aria-label':'Export '+p.name},'Export'),
          this.confirmDel===p.slug?mk('Confirm delete',()=>this.del(p),'danger on'):mk('Delete',()=>{this.confirmDel=p.slug;this.draw()},'danger'))));
    if(this.open===p.slug&&this.diffs[p.slug])item.append(this.diffView(p,this.diffs[p.slug]));
    return item;
  },
  async showDiff(slug){
    try{
      const d=await post('diff',{slug});
      this.diffs={};this.diffs[slug]=d;this.open=slug;this.confirmDel=null;this.say('');this.draw();
    }catch(e){this.say(e.message,true)}
  },
  diffView(p,d){
    const n=d.changes.length;
    const body=n?h('div',{class:'tablewrap'},h('table',{class:'pf-diff'},h('thead',{},h('tr',{},['Setting','Now','Profile'].map(t=>h('th',{},t)))),
      h('tbody',{},d.changes.map(c=>h('tr',{},h('td',{class:'mono'},c.path),h('td',{class:'mono'},val(c.current)),h('td',{class:'mono pf-new'},val(c.profile)))))))
      :h('p',{class:'note'},'Nothing to change: the current settings already match this profile.');
    return h('div',{class:'pf-diffbox'},
      body,
      d.restart?h('p',{class:'pf-warn'},'This restarts the network (interface, mode or device number changes). Outputs pause for a moment.'):null,
      h('div',{class:'pf-row'},
        h('button',{class:'btn primary',onclick:()=>this.apply(p.slug,d.restart)},d.restart?'Confirm and restart network':n?'Confirm apply':'Mark as active'),
        h('button',{class:'btn',onclick:()=>{this.open=null;this.draw()}},'Cancel')));
  },
  async apply(slug,confirm){
    try{
      const r=await post('apply',{slug,confirm:!!confirm});
      this.open=null;this.say('Applied '+r.applied+' change'+(r.applied===1?'':'s')+(r.restart?'; the network is restarting.':'.'));
      await reload();this.draw();
    }catch(e){this.say(e.message,true)}
  },
  async save(overwrite){
    const name=this.name.value.trim();
    if(!name){this.say('Enter a name first.',true);return}
    try{
      const r=await fetch(P+'save',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({name,includeInterface:this.inc.checked,overwrite})});
      const j=await r.json().catch(()=>({}));
      if(r.status===409&&j.exists){
        this.over.classList.remove('hidden');
        this.over.replaceChildren(h('span',{},'“'+name+'” already exists. Replace it with the current settings?'),
          h('button',{class:'btn danger on',onclick:()=>{this.over.classList.add('hidden');this.save(true)}},'Replace'),
          h('button',{class:'btn',onclick:()=>this.over.classList.add('hidden')},'Cancel'));
        return;
      }
      if(!r.ok)throw new Error(j.error||('HTTP '+r.status));
      this.over.classList.add('hidden');this.name.value='';this.say('Saved “'+name+'”.');await reload();this.draw();
    }catch(e){this.say(e.message,true)}
  },
  async rename(p){
    const n=prompt('New name for “'+p.name+'”',p.name);
    if(n==null||!n.trim())return;
    try{await post('rename',{slug:p.slug,name:n.trim()});await reload();this.draw()}catch(e){this.say(e.message,true)}
  },
  async dup(p){try{const r=await post('duplicate',{slug:p.slug});this.say('Created “'+r.name+'”.');await reload();this.draw()}catch(e){this.say(e.message,true)}},
  async del(p){
    this.confirmDel=null;
    try{await post('delete',{slug:p.slug});if(this.open===p.slug)this.open=null;this.say('Deleted “'+p.name+'”.');await reload();this.draw()}catch(e){this.say(e.message,true)}
  },
  async import(inp){
    const f=inp.files[0];inp.value='';if(!f)return;
    if(f.size>65536){this.say(f.name+' is larger than 64 KB.',true);return}
    let doc;
    try{doc=JSON.parse(await f.text())}catch(_){this.say(f.name+' is not valid JSON.',true);return}
    try{
      const r=await post('import',doc);
      this.say('Imported “'+r.name+'”'+(r.warnings&&r.warnings.length?' ('+r.warnings.length+' warning'+(r.warnings.length===1?'':'s')+': '+r.warnings.slice(0,3).join('; ')+')':'.'));
      await reload();this.draw();
    }catch(e){this.say(e.message,true)}
  }
});

// Live control bar: shows the active profile and switches quickly. A profile that restarts the
// network is never applied from here: the user is sent to the page, which shows the diff.
Shunt.registerWidget({slot:'live-controls',order:66,featureId:'profiles',
  mount(el){
    this.sel=h('select',{'aria-label':'Switch venue profile',class:'pf-sel',onchange:()=>this.switch()});
    this.dot=h('span',{class:'pf-dirty hidden',title:'Settings differ from the active profile','aria-label':'Modified'},'●');
    this.hint=h('span',{class:'note pf-hint'},'');
    el.append(h('div',{class:'ctl pf-ctl'},h('span',{class:'lbl'},'Venue'),this.sel,this.dot,this.hint));
    this.sig='';
    this.redraw=()=>this.fill();PR.watchers.push(this.redraw);
    reload().catch(()=>{});
  },
  update(s){track(s);this.fill()},
  fill(){
    if(!this.sel)return;
    const sig=PR.list.map(p=>p.slug+'\n'+p.name).join('\0')+'|'+PR.active;
    if(sig!==this.sig){
      this.sig=sig;
      const opts=[h('option',{value:''},PR.active?'Switch…':'None')];
      for(const p of PR.list)opts.push(h('option',{value:p.slug},p.name));
      this.sel.replaceChildren(...opts);
      this.sel.value='';
      this.sel.disabled=!PR.list.length;
    }
    this.dot.classList.toggle('hidden',!(PR.active&&PR.dirty));
    this.hint.textContent=PR.activeName?'Active: '+PR.activeName:'';
  },
  async switch(){
    const slug=this.sel.value;this.sel.value='';
    if(!slug)return;
    try{await post('apply',{slug});await reload()}
    catch(e){
      if(/confirm|restart/i.test(e.message)){PR.pending=slug;location.hash='#/profiles'}
      else this.hint.textContent=e.message;
    }
  }
});
})();
