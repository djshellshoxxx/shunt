// Diagnostics page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- diagnostics ---------- */
Shunt.registerPage({id:'diagnostics',title:'Diagnostics',order:40,
  mount(root){
    this.sum=h('div',{class:'stats'});this.types=h('tbody');this.devs=h('tbody');
    root.append(h('div',{class:'grid2'},
      h('div',{class:'card'},h('h2',{},'Engine'),this.sum,
        h('button',{class:'btn danger','aria-pressed':'false',id:'recBtn',onclick:()=>api('/api/record',{on:!S.recording})},'Record session to pcapng'),
        h('p',{class:'note'},'Captures are saved under the data directory (captures/). Replay with shunt_cli --replay.')),
      h('div',{class:'card'},h('h2',{},'Packets by type'),h('div',{class:'tablewrap'},h('table',{},h('thead',{},h('tr',{},h('th',{},'Type'),h('th',{},'Count'))),this.types)))),
      h('div',{class:'card',style:'margin-top:20px'},h('h2',{},'Devices'),h('div',{class:'tablewrap'},h('table',{},h('thead',{},h('tr',{},['#','Name','Model','Kind','IP','Last seen'].map(t=>h('th',{},t)))),this.devs))));
  },
  update(s){
    const st=(v,l)=>h('div',{class:'stat'},h('b',{},v),h('span',{},l));
    this.sum.replaceChildren(st(s.interface||'–','Interface'),st(s.timestampSource||'–','Timestamps'),st(s.claim+(s.deviceNumber?' #'+s.deviceNumber:''),'Claim'),st(Math.floor(s.uptimeS)+' s','Uptime'),
      st(s.counters.datagrams,'Datagrams'),st(s.counters.parseErrors,'Parse errors'));
    const t=Object.entries(s.counters.perType);
    this.types.replaceChildren(...(t.length?t.map(([k,v])=>h('tr',{},h('td',{class:'mono'},k),h('td',{},v))):[h('tr',{},h('td',{colspan:2,class:'note'},'No packets yet'))]));
    this.devs.replaceChildren(...s.devices.map(d=>h('tr',{},h('td',{},d.number),h('td',{},d.name),h('td',{},h('span',{class:'badge '+d.badge},d.model)),h('td',{},d.kind),h('td',{class:'mono'},d.ip),h('td',{},(d.lastSeenMs/1000).toFixed(1)+' s'))));
    const b=$('#recBtn');if(b){b.classList.toggle('on',s.recording);b.textContent=s.recording?'● Recording… click to stop':'Record session to pcapng'}
  }
});
