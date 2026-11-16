// Compatibility page (RS-07 section 9), moved verbatim from index.html by FS-00. Original Shunt code.
/* ---------- compatibility ---------- */
const badgeText={full:'Full',tempo:'Tempo only',partial:'Partial',unverified:'Unverified',unsupported:'Unsupported'};
Shunt.registerPage({id:'compat',title:'Compatibility',order:35,
  mount(root){
    const body=h('tbody');
    root.append(h('div',{class:'card'},h('h2',{},'Compatibility'),h('div',{class:'tablewrap'},h('table',{},h('thead',{},h('tr',{},['Model','Support','Notes'].map(t=>h('th',{},t)))),body)),
      h('p',{class:'note'},'Shunt works with PRO DJ LINK compatible players and mixers from AlphaTheta / Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer DJ are trademarks of their respective owners. Shunt is an independent product and is not affiliated with, endorsed, licensed or certified by AlphaTheta Corporation or Pioneer Corporation.')));
    api('/api/compat').then(rows=>body.replaceChildren(...rows.map(r=>h('tr',{},h('td',{},r.model),h('td',{},h('span',{class:'badge '+r.badge},badgeText[r.badge])),h('td',{class:'note'},r.note)))));
  },update(){}
});
