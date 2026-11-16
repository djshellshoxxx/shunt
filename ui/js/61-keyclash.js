// FS-02 key clash alarm: Live valve chip (slot live-valve), mute-pulse button (slot live-controls) and
// the settings card on the Outputs page (slot outputs-osc; always shown so the feature can be switched
// on). Settings go to features.keyclash through POST /api/config; GET /api/f/keyclash/settings returns
// them validated. All text is set with textContent / h(); nothing from the network reaches innerHTML.
// Original Shunt code.
(()=>{
  const KC={f:null,msgs:[],loaded:null,muted:'',pair:''};
  const load=()=>KC.loaded||(KC.loaded=api('/api/f/keyclash/settings').catch(()=>null));
  const num=(v,lo,hi,d)=>{v=Number(v);return Number.isFinite(v)?Math.min(hi,Math.max(lo,v)):d};
  async function save(){
    const f=KC.f;if(!f||!f.enabled)return;
    const cfg=await load()||{};
    const patch={enabled:f.enabled.checked,strict:f.strict.checked,assumeKeyLock:f.lock.checked,
      debounceMs:Math.round(num(f.debounce.value,0,30,1.5)*1000),
      oscEnabled:f.osc.checked,qlabKeyClashCue:Math.round(num(f.qlab.value,0,9999,0)),
      experimentalStatusKey:!!cfg.experimentalStatusKey};
    for(const m of KC.msgs){m.className='kc-msg';m.textContent='Saving…'}
    try{
      await api('/api/config',{features:{keyclash:patch}});
      KC.loaded=null;
      for(const m of KC.msgs){m.className='kc-msg ok';m.textContent='Saved and applied.'}
    }catch(e){for(const m of KC.msgs){m.className='kc-msg err';m.textContent=e.message}}
  }

  Shunt.registerWidget({slot:'outputs-osc',order:61,
    mount(el){
      KC.f={};KC.msgs=[];KC.loaded=null;
      const f=KC.f;
      f.enabled=h('input',{type:'checkbox',id:'kcEnabled'});
      f.strict=h('input',{type:'checkbox',id:'kcStrict'});
      f.lock=h('input',{type:'checkbox',id:'kcLock',checked:true});
      f.osc=h('input',{type:'checkbox',id:'kcOsc',checked:true});
      f.debounce=h('input',{type:'number',id:'kcDebounce',min:'0',max:'30',step:'0.1',value:'1.5'});
      f.qlab=h('input',{type:'number',id:'kcQlab',min:'0',max:'9999',value:'0'});
      const msg=h('div',{class:'kc-msg','aria-live':'polite'});KC.msgs.push(msg);
      el.append(h('div',{class:'kc-box',role:'group','aria-labelledby':'kcTitle'},
        h('h3',{id:'kcTitle',class:'kc-title'},'Key clash alarm'),
        h('label',{class:'check kc-check'},f.enabled,'Warn when the cued deck’s key clashes with what is on air'),
        h('label',{class:'check kc-check'},f.strict,'Strict: also warn for the energy level (same letter, two steps apart)'),
        h('label',{class:'check kc-check'},f.lock,'Assume key lock (ignore pitch; off shifts keys by the pitch fader)'),
        h('label',{class:'check kc-check'},f.osc,'Send /shunt/keyclash over OSC'),
        h('div',{class:'kc-row'},
          h('label',{class:'field'},h('span',{class:'kc-lbl'},'Debounce (seconds)'),f.debounce),
          h('label',{class:'field'},h('span',{class:'kc-lbl'},'QLab cue on alarm (0 = off)'),f.qlab)),
        h('p',{class:'kc-note'},'Keys come from the imported rekordbox XML. The status-packet key bytes are not decoded yet.'),
        h('div',{class:'kc-actions'},h('button',{class:'btn',type:'button',onclick:save},'Apply key clash settings'),msg)));
      load().then(c=>{
        if(!c)return;
        f.enabled.checked=!!c.enabled;f.strict.checked=!!c.strict;f.lock.checked=c.assumeKeyLock!==false;
        f.osc.checked=c.oscEnabled!==false;f.debounce.value=String((c.debounceMs==null?1500:c.debounceMs)/1000);
        f.qlab.value=String(c.qlabKeyClashCue||0);
      });
    },
    update(){}
  });

  const LABEL={perfect:'perfect',compatible:'compatible',energy:'energy',clash:'KEY CLASH',unknown:'key unknown',none:''};
  Shunt.registerWidget({slot:'live-valve',featureId:'keyclash',order:61,
    mount(el){
      this.chip=h('div',{class:'kc-chip none','aria-live':'polite'},'');
      this.link=h('a',{class:'kc-link',href:'#/tracklist'},'Import rekordbox XML for keys');
      this.link.hidden=true;
      el.append(this.chip,this.link);
    },
    update(s){
      const k=s.features&&s.features.keyclash;const c=this.chip;if(!k||!c)return;
      const ref=k.reference&&k.reference.key,cued=k.cued&&k.cued.key;
      const pair=ref&&cued?ref+'|'+cued:'';
      if(pair!==KC.pair){KC.pair=pair}                                   // a new pair re-arms the pulse
      this.link.hidden=!!(k.metaTracks>0);
      let cls='kc-chip',txt;
      if(k.level==='none'){cls+=' none';txt='Key: no pair'}
      else if(k.level==='unknown'){cls+=' none';txt='Key unknown'}
      else if(k.alarm||k.level==='clash'){
        if(k.alarm){cls+=' clash';if(KC.muted!==pair)cls+=' pulse'}else cls+=' clash steady';
        txt='KEY CLASH '+ref+' vs '+cued;
      }
      else if(k.level==='energy'){cls+=' energy';txt=ref+' to '+cued+' energy'}
      else{cls+=' good';txt=ref+' to '+cued+' '+(k.level==='perfect'?'same key':'compatible')}
      c.className=cls;c.textContent=txt;
      c.title=LABEL[k.level]?('Key level: '+LABEL[k.level]):'';
    }
  });

  Shunt.registerWidget({slot:'live-controls',featureId:'keyclash',order:61,
    mount(el){
      const b=h('button',{class:'btn kc-mute',type:'button','aria-pressed':'false',title:'Silence the pulse for this pair of keys',
        onclick:()=>{KC.muted=KC.muted===KC.pair?'':KC.pair;this.paint()}},'Mute key alarm');
      this.btn=b;this.paint=()=>{
        const m=!!KC.pair&&KC.muted===KC.pair;
        b.setAttribute('aria-pressed',String(m));b.classList.toggle('on',m);
        b.textContent=m?'Key alarm muted':'Mute key alarm';
      };
      el.append(b);
    },
    update(s){
      const k=s.features&&s.features.keyclash;if(!k||!this.btn)return;
      this.btn.hidden=!k.alarm;
      this.paint();
    }
  });
})();
