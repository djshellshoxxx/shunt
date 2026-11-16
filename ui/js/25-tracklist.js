// Tracklist page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- tracklist ---------- */
Shunt.registerPage({id:'tracklist',title:'Tracklist',order:25,
  mount(root){
    this.body=h('tbody');this.rows=-1;this.imp=h('div',{class:'msg','aria-live':'polite'});
    root.append(h('div',{class:'card'},h('h2',{},'Tracklist'),
      h('div',{style:'display:flex;gap:10px;flex-wrap:wrap;margin-bottom:14px'},
        h('button',{class:'btn',onclick:()=>api('/api/setstart')},'Set start marker now'),
        [['csv','CSV'],['cue','CUE'],['txt','Timestamps'],['ndjson','NDJSON'],['report','Report CSV']].map(([f,t])=>h('a',{class:'btn',style:'text-decoration:none',href:'/api/export?format='+f,download:''},t)),
        h('label',{class:'btn',style:'cursor:pointer'},'Import rekordbox XML',h('input',{type:'file',accept:'.xml,text/xml',class:'sr',onchange:async e=>{
          const f=e.target.files[0];if(!f)return;this.imp.className='msg';this.imp.textContent='Importing '+f.name+'…';
          try{const r=await fetch('/api/rekordbox-xml',{method:'POST',body:await f.text()});const j=await r.json();if(!r.ok)throw new Error(j.error);
            this.imp.className='msg ok';this.imp.textContent='Matched metadata for '+j.tracks+' tracks in the file.';this.rows=-1}
          catch(x){this.imp.className='msg err';this.imp.textContent=x.message}e.target.value=''}})),
        h('button',{class:'btn',style:'margin-left:auto',onclick:async()=>{if(confirm('Start a new session? The current rows stay in the saved NDJSON file.')){await api('/api/session/clear',{});this.rows=-1}}},'New session')),
      this.imp,
      h('div',{class:'tablewrap'},h('table',{},h('thead',{},h('tr',{},['#','Start','End','Deck','Track','Slot','Played'].map(t=>h('th',{},t)))),this.body)),
      h('p',{class:'note'},'Rows show the rekordbox id until you import a rekordbox XML export (File → Export Collection in xml format); names then fill in everywhere, including exports. A track counts as played after 30 s or 20 percent on air.')));
  },
  update(s){
    if(s.tracks===this.rows)return;this.rows=s.tracks;
    api('/api/tracklist').then(t=>{
      this.body.replaceChildren(...(t.rows.length?t.rows.map((r,i)=>h('tr',{},h('td',{},i+1),h('td',{class:'mono'},fmtTime(r.startedAt)),h('td',{class:'mono'},fmtTime(r.endedAt)),h('td',{},'#'+r.deck),
        h('td',{},r.title||r.artist?((r.artist||'ID')+' - '+(r.title||'ID')):('ID '+r.rekordboxId)),h('td',{},r.slot),h('td',{},Math.round(r.playedPct)+'%'))):[h('tr',{},h('td',{colspan:7,class:'note'},'Nothing logged yet.'))]));
    });
  }
});
