// FS-01 on-air routing and clock mute: settings editor in the Outputs MIDI card (slot
// outputs-midi; always shown so the feature can be switched on), OSC options (slot outputs-osc)
// and the ON AIR / MUTED chip on the Live valve (slot live-valve). Settings go to
// features.onair through POST /api/config; GET /api/f/onair/settings returns them validated.
// Original Shunt code.
(()=>{
  const OA={f:null,msgs:[],loaded:null};
  const load=()=>OA.loaded||(OA.loaded=api('/api/f/onair/settings').catch(()=>null));
  const num=(v,lo,hi,d)=>{v=Number(v);return Number.isFinite(v)?Math.min(hi,Math.max(lo,v)):d};
  async function save(){
    const f=OA.f;if(!f)return;
    const cfg=await load()||{};
    const map=f.map.map(s=>+s.value);
    const patch={enabled:f.enabled.checked,midiMute:f.mute.checked?'stop':'off',
      holdMs:Math.round(num(f.hold.value,0.5,30,4)*1000),unmuteWith:f.unmute.value,channelMap:map,
      oscPerDeck:f.perDeck?f.perDeck.checked:cfg.oscPerDeck!==false,
      qlabOnAirCue:f.qOn?Math.round(num(f.qOn.value,0,9999,0)):(cfg.qlabOnAirCue||0),
      qlabMuteCue:f.qMute?Math.round(num(f.qMute.value,0,9999,0)):(cfg.qlabMuteCue||0)};
    for(const m of OA.msgs){m.className='oa-msg';m.textContent='Saving…'}
    try{
      await api('/api/config',{features:{onair:patch}});
      OA.loaded=null;
      for(const m of OA.msgs){m.className='oa-msg ok';m.textContent='Saved and applied.'}
    }catch(e){for(const m of OA.msgs){m.className='oa-msg err';m.textContent=e.message}}
  }
  const msgEl=()=>{const m=h('div',{class:'oa-msg','aria-live':'polite'});OA.msgs.push(m);return m};
  const applyBtn=()=>h('button',{class:'btn',type:'button',onclick:save},'Apply on-air settings');

  Shunt.registerWidget({slot:'outputs-midi',order:60,
    mount(el){
      OA.f=OA.f||{};OA.msgs=[];OA.loaded=null;
      const f=OA.f;
      f.enabled=h('input',{type:'checkbox',id:'oaEnabled'});
      f.mute=h('input',{type:'checkbox',id:'oaMute'});
      f.hold=h('input',{type:'number',id:'oaHold',min:'0.5',max:'30',step:'0.5',value:'4'});
      f.unmute=h('select',{id:'oaUnmute'},h('option',{value:'start'},'Start'),h('option',{value:'continue'},'Continue'));
      f.map=[1,2,3,4,5,6].map(d=>h('select',{'aria-label':'Mixer channel for deck '+d},[1,2,3,4,5,6].map(c=>h('option',{value:String(c)},'Ch '+c))));
      const box=h('div',{class:'oa-box',role:'group','aria-labelledby':'oaTitle'},
        h('h3',{id:'oaTitle',class:'oa-title'},'On air'),
        h('label',{class:'check oa-check'},f.enabled,'Follow on-air state (mixer and player bits)'),
        h('label',{class:'check oa-check'},f.mute,'Stop the clock when nothing is on air'),
        h('div',{class:'oa-row'},
          h('label',{class:'field'},h('span',{class:'oa-lbl'},'Hold before stopping (seconds)'),f.hold),
          h('label',{class:'field'},h('span',{class:'oa-lbl'},'Restart with'),f.unmute)),
        h('fieldset',{class:'oa-map'},h('legend',{class:'oa-lbl'},'Mixer channel of each deck'),
          f.map.map((s,i)=>h('label',{class:'oa-cell'},h('span',{class:'oa-lbl'},'Deck '+(i+1)),s))),
        h('p',{class:'oa-note'},'Mixer on-air packets win while they arrive; otherwise each player’s own on-air bit is used. Stop is sent once; the restart lands on the next bar.'),
        h('div',{class:'oa-actions'},applyBtn(),msgEl()));
      el.append(box);
      load().then(c=>{
        if(!c)return;
        f.enabled.checked=!!c.enabled;f.mute.checked=c.midiMute==='stop';
        f.hold.value=String((c.holdMs||4000)/1000);f.unmute.value=c.unmuteWith==='continue'?'continue':'start';
        (c.channelMap||[]).forEach((v,i)=>{if(f.map[i])f.map[i].value=String(v)});
      });
    },
    update(){}
  });

  Shunt.registerWidget({slot:'outputs-osc',order:60,
    mount(el){
      OA.f=OA.f||{};
      const f=OA.f;
      f.perDeck=h('input',{type:'checkbox',id:'oaPerDeck',checked:true});
      f.qOn=h('input',{type:'number',id:'oaQOn',min:'0',max:'9999',value:'0'});
      f.qMute=h('input',{type:'number',id:'oaQMute',min:'0',max:'9999',value:'0'});
      el.append(h('div',{class:'oa-box',role:'group','aria-label':'On-air OSC'},
        h('h3',{class:'oa-title'},'On-air OSC'),
        h('label',{class:'check oa-check'},f.perDeck,'Per-deck /shunt/onair messages'),
        h('div',{class:'oa-row'},
          h('label',{class:'field'},h('span',{class:'oa-lbl'},'QLab cue when on air (0 = off)'),f.qOn),
          h('label',{class:'field'},h('span',{class:'oa-lbl'},'QLab cue on clock stop (0 = off)'),f.qMute)),
        h('p',{class:'oa-note'},'Generic and MagicQ also get /shunt/onair/any and /shunt/mute.'),
        h('div',{class:'oa-actions'},applyBtn(),msgEl())));
      load().then(c=>{
        if(!c)return;
        f.perDeck.checked=c.oscPerDeck!==false;f.qOn.value=String(c.qlabOnAirCue||0);f.qMute.value=String(c.qlabMuteCue||0);
      });
    },
    update(){}
  });

  const fmtSince=ms=>{const s=Math.floor(ms/1000);return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
  Shunt.registerWidget({slot:'live-valve',featureId:'onair',order:60,
    mount(el){this.chip=h('div',{class:'oa-chip','aria-live':'polite'},'');el.append(this.chip)},
    update(s){
      const o=s.features&&s.features.onair;const c=this.chip;if(!o||!c)return;
      const on=(o.decks||[]).filter(d=>d.on).map(d=>d.deck);
      let cls='oa-chip',txt;
      if(o.muted){cls+=' muted';txt='MUTED '+fmtSince(o.sinceMs||0)}
      else if(on.length){cls+=' on';txt='ON AIR '+on.join(', ')}
      else if(!o.dataSeen){cls+=' none';txt='NO ON-AIR DATA'}
      else txt='OFF AIR';
      c.className=cls;c.textContent=txt;
      c.title=o.source==='mixer'?'Source: mixer':o.source==='players'?'Source: player on-air bits':'No source';
    }
  });
})();
