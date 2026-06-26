/* SPDX-License-Identifier: MIT */
/*
 * server.c — polar_doctor_web (P0) : mini serveur HTTP zéro-dépendance
 * servant une SPA (diagramme polaire en Canvas, lecture seule) + une API JSON.
 *
 * Cœur HTTP repris de n2k-mux/src/web.c (mono-client séquentiel, tampons fixes,
 * auth HTTP Basic optionnelle). Réutilise le cœur C de polar_doctor (polar_data.c)
 * pour charger un .pol et l'exposer en JSON.
 *
 *   GET /            → page unique (HTML/CSS/JS embarqués), diagramme Canvas
 *   GET /api/polar   → la polaire chargée, en JSON
 *
 * Usage : polar_doctor_web [fichier.pol] [--port N] [--bind ADDR] [--auth user:pass]
 */

#include "libpolar.h"

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <poll.h>
#include <netdb.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX   131072   /* en-têtes + corps POST (.pol édité) */
#define JSON_MAX  262144   /* polaire en JSON (worst case ~110 Ko) */

static PolarData    g_polar;
static int          g_loaded = 0;
static const char  *g_auth = NULL;     /* "user:pass" attendu (NULL = pas d'auth) */
static char         g_auth_b64[352];

/* Bateau = dossier de .pol (liste POSIX ; le boat.cfg viendra avec libpolar). */
#define MAXPOL 64
static char g_boat_name[128] = "";
static char g_boat_dir[512]  = "";   // dossier-bateau (pour re-scan après sauvegarde live)
static char g_pol_paths[MAXPOL][512];
static char g_pol_names[MAXPOL][128];
static int  g_npol = 0;
static int  g_cur  = 0;

/* Grilles et état de la capture live (déclarés tôt : utilisés par serve_select). */
static polar_grid_t g_grids[BOAT_MAX_POLARS];  /* 1, ou 1 par polaire si routage */
static int   g_ng = 1, g_routing = 0, g_disp = 0;
static long  g_gadd[BOAT_MAX_POLARS] = {0};    /* points ajoutés par grille (sauvegarde) */
static char  g_cur_main[BOAT_TERM_LEN] = "", g_cur_head[BOAT_TERM_LEN] = "", g_cur_sea[BOAT_TERM_LEN] = "";
static int def_index_for_selected(void);

/* ------------------------------------------------------------------ SPA --- */
static const char PAGE[] =
"<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>Polar Doctor</title>\n"
"<style>\n"
":root{--bg:#0f1216;--fg:#d8dee5;--panel:#161b22;--border:#2a313a;--muted:#8b949e;--accent:#58a6ff;--btn:#21262d;--active:#1f6feb;--inbg:#0b0e12}\n"
"body.light{--bg:#fff;--fg:#1b2430;--panel:#f3f5f8;--border:#cfd8e3;--muted:#5b6675;--accent:#0b62d6;--btn:#e6ebf2;--active:#1f6feb;--inbg:#fff}\n"
"*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}\n"
"header{background:var(--panel);padding:.6em 1em;border-bottom:1px solid var(--border);display:flex;gap:1em;align-items:center}\n"
"header b{color:var(--accent)}\n"
".hbtn{background:var(--btn);color:var(--fg);border:1px solid var(--border);padding:.25em .6em;border-radius:6px;cursor:pointer;font-size:.85em}\n"
"main{padding:1em;display:flex;gap:1em;flex-wrap:wrap}\n"
"#wrap{flex:1 1 480px;min-width:320px}\n"
"#cv{width:100%;height:84vh;background:var(--panel);border:1px solid var(--border);border-radius:8px}\n"
"aside{flex:0 0 220px}\n"
".card{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:.6em .9em;margin-bottom:.8em}\n"
".card h3{margin:.2em 0 .5em;font-size:.9em;color:var(--accent)}\n"
"label{display:block;font-size:.85em;margin:.3em 0}\n"
"select{width:100%;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.2em}\n"
".lg{font-size:.85em}.lg div{display:flex;align-items:center;gap:.5em;margin:.15em 0}\n"
".sw{width:14px;height:14px;border-radius:3px;flex:0 0 auto}\n"
"small{color:var(--muted)}\n"
"header nav{display:flex;gap:.3em;margin-left:1em}\n"
"header nav button{background:var(--btn);color:var(--fg);border:1px solid var(--border);padding:.25em .7em;border-radius:6px;cursor:pointer}\n"
"header nav button.on{background:var(--active);color:#fff;border-color:var(--active)}\n"
".dt{border-collapse:collapse;font-size:.85em}.dt th,.dt td{border:1px solid var(--border);padding:2px 5px;text-align:center}\n"
".dt input.dc{width:4.4em;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:3px;text-align:right;font-variant-numeric:tabular-nums}\n"
".delx{background:none;border:0;color:var(--muted);cursor:pointer;font-size:.75em;padding:0}\n"
"@media print{header nav,#prt,#lang,#theme,aside{display:none!important}#wrap{flex:1 1 100%}}\n"
"</style></head><body>\n"
"<header><b>Polar Doctor</b><small id='fn'></small>\n"
"<nav><button data-v='diag' class='on' data-i18n='tabdiag'>Diagramme</button><button data-v='data' data-i18n='tabdata'>Données</button><button data-v='vmg' data-i18n='tabvmg'>VMG</button></nav>\n"
"<span style='margin-left:auto;display:flex;gap:.5em'>"
"<button id='prt' class='hbtn' title='Imprimer / PDF'>⎙</button>"
"<button id='lang' class='hbtn'></button><button id='theme' class='hbtn'></button></span></header>\n"
"<main id='mdiag'>\n"
"<div id='wrap'><canvas id='cv'></canvas></div>\n"
"<aside>\n"
"<div class='card'><h3 data-i18n='boat'>Bateau</h3>\n"
"<div id='bname' style='font-size:.9em;margin-bottom:.3em'></div>\n"
"<select id='polsel'></select></div>\n"
"<div class='card'><h3 data-i18n='range'>Plage TWS</h3>\n"
"<label><span data-i18n='from'>De</span> <select id='from'></select></label>\n"
"<label><span data-i18n='to'>à</span> <select id='to'></select></label></div>\n"
"<div class='card'><h3 data-i18n='dyn'>Mode dynamique</h3>\n"
"<label><input type=checkbox id='dyn'> <span data-i18n='dyn_on'>Activer</span></label>\n"
"<label><span data-i18n='tws1'>TWS</span> <input type=number id='dtws' value='10' min='0' step='0.5' style='width:5em'> <span data-i18n='kn'>nœuds</span></label>\n"
"<div id='read' style='font-size:.85em;margin-top:.4em'></div></div>\n"
"<div class='card'><h3 data-i18n='live'>Live</h3>\n"
"<label><span data-i18n='source'>Source</span> <select id='lvsrc'><option value='udp'>NMEA UDP</option><option value='tcp'>NMEA TCP</option><option value='vdr'>VDR qtVlm</option></select></label>\n"
"<label><input id='lvaddr' value='10110' style='width:9em' title='UDP: port · TCP: hôte:port · VDR: chemin .db'></label>\n"
"<button class='hbtn' id='lvbtn' data-i18n='start'>Démarrer</button> <button class='hbtn' id='lvmot' data-i18n='moteur'>Moteur</button>\n"
"<div id='lvstate' style='margin-top:.4em;display:none'>\n"
"<label><span data-i18n='main1'>GV</span> <select id='lvmain'></select></label>\n"
"<label><span data-i18n='head1'>Voile av.</span> <select id='lvhead'></select></label>\n"
"<label><span data-i18n='sea1'>Mer</span> <select id='lvsea'></select></label></div>\n"
"<div id='lvinfo' style='font-size:.85em;margin-top:.3em'></div></div>\n"
"<div class='card'><h3 data-i18n='legend'>Légende (TWS)</h3><div id='leg' class='lg'></div></div>\n"
"<div class='card'><small id='info'></small></div>\n"
"</aside></main>\n"
"<div id='mdata' style='display:none;padding:1em'>\n"
"<div style='margin-bottom:.6em'><button class='hbtn' id='btnSave' data-i18n='save1'>Enregistrer</button> "
"<button class='hbtn' id='btnAddTwa' data-i18n='addtwa'>+ TWA</button> "
"<button class='hbtn' id='btnAddTws' data-i18n='addtws'>+ TWS</button> "
"<span id='dmsg' style='font-size:.85em;color:var(--muted)'></span></div>\n"
"<div style='margin:.2em 0 .8em;padding:.5em;border:1px solid var(--border);border-radius:6px'>\n"
"<b data-i18n='import'>Import fichiers</b><br>\n"
"<textarea id='imppaths' rows='2' placeholder='/chemin/nav.nmea (un par ligne)' style='width:100%;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px'></textarea>\n"
"<button class='hbtn' id='btnCreate' data-i18n='create1'>Créer</button> <button class='hbtn' id='btnUpdate' data-i18n='update1'>Mettre à jour</button> <span id='impmsg' style='font-size:.85em;color:var(--muted)'></span></div>\n"
"<div id='dtable' style='overflow:auto'></div></div>\n"
"<div id='mvmg' style='display:none;padding:1em'><div id='vmgtable' style='overflow:auto'></div></div>\n"
"<script>\n"
"let lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"let theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"const L={fr:{boat:'Bateau',range:'Plage TWS',from:'De',to:'à',legend:'Légende (TWS)',kn:'nœuds',empty:'Aucune polaire chargée.',max:'Vitesse max',dyn:'Mode dynamique',dyn_on:'Activer',tws1:'TWS',live:'Live',source:'Source',start:'Démarrer',stop:'Arrêter',moteur:'Moteur',main1:'GV',head1:'Voile av.',sea1:'Mer',tabdiag:'Diagramme',tabdata:'Données',save1:'Enregistrer',addtwa:'+ TWA',addtws:'+ TWS',import:'Import fichiers',create1:'Créer',update1:'Mettre à jour',tabvmg:'VMG',vmgup:'Près',vmgdn:'Portant'},\n"
"en:{boat:'Boat',range:'TWS range',from:'From',to:'to',legend:'Legend (TWS)',kn:'knots',empty:'No polar loaded.',max:'Max speed',dyn:'Dynamic mode',dyn_on:'Enable',tws1:'TWS',live:'Live',source:'Source',start:'Start',stop:'Stop',moteur:'Engine',main1:'Main',head1:'Headsail',sea1:'Sea',tabdiag:'Diagram',tabdata:'Data',save1:'Save',addtwa:'+ TWA',addtws:'+ TWS',import:'Import files',create1:'Create',update1:'Update',tabvmg:'VMG',vmgup:'Upwind',vmgdn:'Downwind'}};\n"
"const T=k=>(L[lang]&&L[lang][k]!=null)?L[lang][k]:k;\n"
"function i18n(){document.querySelectorAll('[data-i18n]').forEach(e=>e.textContent=T(e.dataset.i18n));document.documentElement.lang=lang;}\n"
"const $=s=>document.querySelector(s);\n"
"let P=null;\n"
"const PAL=['rgb(51,102,255)','rgb(0,204,0)','rgb(230,217,0)','rgb(255,0,255)','rgb(255,128,102)','rgb(166,166,166)','rgb(255,140,0)','rgb(102,153,255)','rgb(255,102,179)','rgb(153,153,51)','rgb(102,255,128)','rgb(0,204,204)'];\n"  /* palette GTK tws_palette_color */
"function col(i){return PAL[((i%12)+12)%12];}\n"
"function drawCurve(x,p,au,ad,base){const t=0.5;\n"  /* t=0.5 + réflexion aux bords = diagram.c (GTK) ; couleur par segment (VMG) */
" for(let i=0;i<p.length-1;i++){const p1=p[i],p2=p[i+1];\n"
"  const p0=i?p[i-1]:[2*p1[0]-p2[0],2*p1[1]-p2[1]];\n"
"  const p3=(i+2<p.length)?p[i+2]:[2*p2[0]-p1[0],2*p2[1]-p1[1]];\n"
"  const c1x=p1[0]+(p2[0]-p0[0])*t/6,c1y=p1[1]+(p2[1]-p0[1])*t/6,c2x=p2[0]-(p3[0]-p1[0])*t/6,c2y=p2[1]-(p3[1]-p1[1])*t/6;\n"
"  const mid=(p1[2]+p2[2])/2;x.strokeStyle=(mid>=au&&mid<=ad)?base:'#e00';\n"  /* utile = couleur TWS, dégradé = rouge */
"  x.beginPath();x.moveTo(p1[0],p1[1]);x.bezierCurveTo(c1x,c1y,c2x,c2y,p2[0],p2[1]);x.stroke();}}\n"
"function opt(sel,arr,val){sel.innerHTML=arr.map((v,i)=>'<option value='+i+(i===val?' selected':'')+'>'+v+' '+T('kn')+'</option>').join('');}\n"
"function shownIdx(){let a=+$('#from').value,b=+$('#to').value;if(a>b){const t=a;a=b;b=t;}const r=[];for(let i=a;i<=b;i++)r.push(i);return r;}\n"
"let DYN=null,CUR=null,LIVE=null,liveTimer=null,G={cx:0,cy:0,R:0,top:1};\n"
"function interpBS(c,twa){const p=c.pts;if(!p||!p.length)return 0;if(twa<=p[0][0])return p[0][1];if(twa>=p[p.length-1][0])return p[p.length-1][1];\n"
" for(let i=0;i<p.length-1;i++)if(twa>=p[i][0]&&twa<=p[i+1][0]){const f=(twa-p[i][0])/((p[i+1][0]-p[i][0])||1);return p[i][1]+f*(p[i+1][1]-p[i][1]);}return 0;}\n"
"function draw(){const c=$('#cv'),x=c.getContext('2d');const W=c.width=c.clientWidth,H=c.height=c.clientHeight;\n"
" const cs=getComputedStyle(document.body),bd=cs.getPropertyValue('--border'),mu=cs.getPropertyValue('--muted');\n"
" x.clearRect(0,0,W,H);if(!P||!P.tws.length){x.fillStyle=mu;x.fillText(T('empty'),20,30);return;}\n"
" const dyn=$('#dyn').checked&&DYN;\n"
" const liveOn=LIVE&&LIVE.on&&LIVE.curves&&LIVE.curves.length;\n"
" let items;\n"
" if(dyn)items=[{c:DYN,color:'rgb(0,204,0)',tws:DYN.tws}];\n"
" else if(liveOn)items=LIVE.curves.map((c,i)=>({c:c,color:col(i),tws:LIVE.tws[i]}));\n"
" else items=shownIdx().map(s=>({c:P.curves[s],color:col(s),tws:P.tws[s]})).filter(o=>o.c);\n"
" let mx=0;for(const it of items)for(const q of it.c.pts)mx=Math.max(mx,q[1]);\n"
" if(LIVE&&LIVE.pts){for(const q of LIVE.pts)mx=Math.max(mx,q[1]);if(LIVE.cur)mx=Math.max(mx,LIVE.cur[1]);}if(mx<=0)mx=1;\n"
" const ring=2;const top=Math.max(ring,Math.ceil(mx/ring)*ring);\n"  /* cercles tous les 2 nœuds */
" const cx=W*0.14,cy=H*0.5,R=Math.min(H*0.46,W*0.82);G={cx:cx,cy:cy,R:R,top:top};\n"
" const px=(twa,bsp)=>[cx+R*bsp/top*Math.sin(twa*Math.PI/180),cy-R*bsp/top*Math.cos(twa*Math.PI/180)];\n"
" x.strokeStyle=bd;x.fillStyle=mu;x.font='12px system-ui';x.textAlign='left';\n"
" for(let r=ring;r<=top+0.001;r+=ring){x.beginPath();for(let t=0;t<=180;t+=2){const p=px(t,r);t===0?x.moveTo(p[0],p[1]):x.lineTo(p[0],p[1]);}x.stroke();const lp=px(0,r);x.fillText(r,lp[0]+3,lp[1]+3);}\n"
" for(let t=0;t<=180;t+=15){x.beginPath();x.moveTo(cx,cy);const p=px(t,top);x.lineTo(p[0],p[1]);x.stroke();const lp=px(t,top*1.06);x.fillText(t+'°',lp[0]-6,lp[1]);}\n"
" x.lineWidth=2;items.forEach(it=>{if(it.c.pts.length<2)return;const pts=it.c.pts.map(q=>{const xy=px(q[0],q[1]);return [xy[0],xy[1],q[0]];});drawCurve(x,pts,it.c.a_up,it.c.a_dn,it.color);});x.lineWidth=1;\n"
" if(LIVE&&LIVE.pts){x.fillStyle='rgba(140,140,140,.55)';for(const q of LIVE.pts){const p=px(q[0],q[1]);x.beginPath();x.arc(p[0],p[1],2,0,7);x.fill();}\n"
"  if(LIVE.cur&&LIVE.cur[1]>0){const p=px(LIVE.cur[0],LIVE.cur[1]);x.fillStyle='#e00';x.beginPath();x.arc(p[0],p[1],4.5,0,7);x.fill();}}\n"
" if(dyn&&CUR){const bs=CUR.bs,tr=CUR.twa*Math.PI/180,tws=DYN.tws;\n"
"  const aws=Math.sqrt(bs*bs+tws*tws+2*bs*tws*Math.cos(tr)),awa=Math.atan2(tws*Math.sin(tr),bs+tws*Math.cos(tr))*180/Math.PI,vmg=bs*Math.cos(tr);\n"
"  const p=px(CUR.twa,bs);x.strokeStyle='#1f6feb';x.lineWidth=1.5;x.beginPath();x.moveTo(cx,cy);x.lineTo(p[0],p[1]);x.stroke();x.fillStyle='#1f6feb';x.beginPath();x.arc(p[0],p[1],3,0,7);x.fill();x.lineWidth=1;\n"
"  $('#read').innerHTML='TWA <b>'+CUR.twa+'°</b> · AWA '+awa.toFixed(0)+'° · AWS '+aws.toFixed(1)+' · BS <b>'+bs.toFixed(2)+'</b> · VMG '+vmg.toFixed(2);}\n"
" else if(dyn)$('#read').textContent='';\n"
" $('#leg').innerHTML=items.map(it=>'<div><span class=sw style=\"background:'+it.color+'\"></span>'+it.tws+' '+T('kn')+'</div>').join('');\n"
" $('#info').textContent=T('max')+' : '+mx.toFixed(2)+' '+T('kn');\n"
"}\n"
"async function load(){try{const r=await fetch('/api/polar');P=await r.json();}catch(e){P=null;}\n"
" $('#fn').textContent=P&&P.filename?(' — '+P.filename):'';\n"
" if(P&&P.tws.length){opt($('#from'),P.tws,0);opt($('#to'),P.tws,P.tws.length-1);}draw();}\n"
"$('#from').onchange=draw;$('#to').onchange=draw;addEventListener('resize',draw);\n"
"async function loadBoat(){try{const b=await fetch('/api/boat').then(r=>r.json());\n"
" $('#bname').textContent=b.name||'';\n"
" $('#polsel').innerHTML=(b.polars||[]).map((p,i)=>'<option value='+i+(i===b.current?' selected':'')+'>'+p+'</option>').join('');\n"
" $('#polsel').style.display=(b.polars&&b.polars.length>1)?'':'none';\n"
" const inv=(b.mains&&b.mains.length)||(b.heads&&b.heads.length)||(b.seas&&b.seas.length);\n"
" $('#lvstate').style.display=inv?'':'none';\n"
" const fillSel=(id,arr,cur)=>{$(id).innerHTML='<option value=\"\">—</option>'+(arr||[]).map(v=>'<option'+(v===cur?' selected':'')+'>'+v+'</option>').join('');};\n"
" if(inv){fillSel('#lvmain',b.mains,b.cur_main);fillSel('#lvhead',b.heads,b.cur_head);fillSel('#lvsea',b.seas,b.cur_sea);}\n"
" }catch(e){}}\n"
"$('#polsel').onchange=async e=>{await fetch('/api/select?i='+e.target.value);await load();if($('#dyn').checked)loadDyn();if($('#lvbtn').dataset.on==='1')pollLive();};\n"
"async function loadDyn(){const v=parseFloat($('#dtws').value)||0;try{const r=await fetch('/api/curve?tws='+v);DYN=await r.json();}catch(e){DYN=null;}CUR=null;draw();}\n"
"$('#dyn').onchange=()=>{if($('#dyn').checked)loadDyn();else{DYN=null;CUR=null;draw();}};\n"
"$('#dtws').onchange=()=>{if($('#dyn').checked)loadDyn();};\n"
"$('#cv').addEventListener('mousemove',e=>{if(!($('#dyn').checked&&DYN))return;const r=e.target.getBoundingClientRect();\n"
" const dx=(e.clientX-r.left)-G.cx,dy=G.cy-(e.clientY-r.top);let twa=Math.atan2(dx,dy)*180/Math.PI;twa=Math.max(0,Math.min(180,Math.round(twa)));\n"
" CUR={twa:twa,bs:interpBS(DYN,twa)};draw();});\n"
"$('#cv').addEventListener('mouseleave',()=>{if(CUR){CUR=null;draw();}});\n"
"function startPoll(){if(!liveTimer)liveTimer=setInterval(pollLive,1000);}\n"
"function stopPoll(){if(liveTimer){clearInterval(liveTimer);liveTimer=null;}}\n"
"async function pollLive(){try{LIVE=await fetch('/api/live').then(r=>r.json());}catch(e){LIVE=null;}\n"
" if(LIVE&&LIVE.on)$('#lvinfo').textContent='● live · '+LIVE.count+' pts'+(LIVE.cur?(' · TWA '+Math.round(LIVE.cur[0])+'° · BS '+LIVE.cur[1].toFixed(2)):'');\n"
" else $('#lvinfo').textContent=(LIVE&&LIVE.saved)?('✓ '+LIVE.saved.split('/').pop()):'';\n"
" const on=!!(LIVE&&LIVE.on);$('#lvbtn').textContent=on?T('stop'):T('start');$('#lvbtn').dataset.on=on?'1':'';\n"
" const mot=!!(LIVE&&LIVE.moteur);$('#lvmot').dataset.on=mot?'1':'';$('#lvmot').style.background=mot?'var(--active)':'';$('#lvmot').style.color=mot?'#fff':'';\n"
" draw();}\n"
"$('#lvbtn').onclick=async()=>{if($('#lvbtn').dataset.on==='1'){await fetch('/api/live/stop');stopPoll();await loadBoat();await pollLive();}\n"
" else{await fetch('/api/live/start?src='+$('#lvsrc').value+'&addr='+encodeURIComponent($('#lvaddr').value));startPoll();await pollLive();}};\n"
"$('#lvmot').onclick=async()=>{const on=$('#lvmot').dataset.on==='1'?0:1;await fetch('/api/live/moteur?on='+on);await pollLive();};\n"
"$('#lvsrc').onchange=()=>{$('#lvaddr').value=$('#lvsrc').value==='vdr'?'/home/ozolli/.qtVlm/vdrs/vdr.db':'10110';};\n"
"function sendState(){fetch('/api/live/state?main='+encodeURIComponent($('#lvmain').value)+'&head='+encodeURIComponent($('#lvhead').value)+'&sea='+encodeURIComponent($('#lvsea').value)).then(()=>pollLive());}\n"
"$('#lvmain').onchange=sendState;$('#lvhead').onchange=sendState;$('#lvsea').onchange=sendState;\n"
"function renderTable(){if(!P||!P.twa){$('#dtable').innerHTML='';return;}\n"
" let h='<table class=dt><tr><th></th>';\n"
" P.tws.forEach((t,k)=>h+='<th>'+t+'<br><button class=delx data-c='+k+'>✕</button></th>');h+='</tr>';\n"
" P.twa.forEach((a,ai)=>{h+='<tr><th>'+a+'°<br><button class=delx data-r='+ai+'>✕</button></th>';\n"
"  P.tws.forEach((t,k)=>{const v=(P.bsp[ai]&&P.bsp[ai][k]!=null)?+P.bsp[ai][k]:0;h+='<td><input class=dc data-r='+ai+' data-c='+k+' value=\"'+v.toFixed(2)+'\"></td>';});h+='</tr>';});\n"
" $('#dtable').innerHTML=h+'</table>';}\n"
"$('#dtable').addEventListener('input',e=>{if(!e.target.classList.contains('dc'))return;P.bsp[+e.target.dataset.r][+e.target.dataset.c]=parseFloat(e.target.value)||0;});\n"
"$('#dtable').addEventListener('click',e=>{const t=e.target;if(!t.classList.contains('delx'))return;\n"
" if(t.dataset.c!==undefined){const c=+t.dataset.c;P.tws.splice(c,1);P.bsp.forEach(r=>r.splice(c,1));}\n"
" else if(t.dataset.r!==undefined){const r=+t.dataset.r;P.twa.splice(r,1);P.bsp.splice(r,1);}renderTable();});\n"
"$('#btnAddTwa').onclick=()=>{const v=parseInt(prompt('TWA (0-180)'),10);if(isNaN(v)||v<0||v>180||P.twa.includes(v))return;let i=0;while(i<P.twa.length&&P.twa[i]<v)i++;P.twa.splice(i,0,v);P.bsp.splice(i,0,P.tws.map(()=>0));renderTable();};\n"
"$('#btnAddTws').onclick=()=>{const v=parseInt(prompt('TWS (kn)'),10);if(isNaN(v)||v<=0||P.tws.includes(v))return;let i=0;while(i<P.tws.length&&P.tws[i]<v)i++;P.tws.splice(i,0,v);P.bsp.forEach(r=>r.splice(i,0,0));renderTable();};\n"
"function buildPol(){let s='TWA\\\\TWS;0;'+P.tws.join(';')+'\\n';P.twa.forEach((a,ai)=>{s+=a+';0.00';P.tws.forEach((t,k)=>{s+=';'+((P.bsp[ai]&&P.bsp[ai][k]!=null)?+P.bsp[ai][k]:0).toFixed(2);});s+='\\n';});return s;}\n"
"$('#btnSave').onclick=async()=>{const r=await fetch('/api/save',{method:'POST',body:buildPol()});const d=await r.json().catch(()=>({}));if(d.ok){await load();renderTable();$('#dmsg').textContent='✓';}else $('#dmsg').textContent='✗';};\n"
"async function doImport(upd){const paths=$('#imppaths').value.trim();if(!paths)return;$('#impmsg').textContent='…';\n"
" const r=await fetch('/api/import?mode='+(upd?'update':'create'),{method:'POST',body:paths});const d=await r.json().catch(()=>({}));\n"
" if(d.ok){$('#impmsg').textContent='✓ '+d.files+' fich., '+d.points+' pts → '+(d.saved.split('/').pop());await loadBoat();await load();renderTable();}else $('#impmsg').textContent='✗';}\n"
"$('#btnCreate').onclick=()=>doImport(false);$('#btnUpdate').onclick=()=>doImport(true);\n"
"document.querySelectorAll('header nav button').forEach(b=>b.onclick=()=>{const v=b.dataset.v;$('#mdiag').style.display=v==='diag'?'':'none';$('#mdata').style.display=v==='data'?'':'none';$('#mvmg').style.display=v==='vmg'?'':'none';document.querySelectorAll('header nav button').forEach(x=>x.classList.toggle('on',x===b));if(v==='data')renderTable();if(v==='vmg')renderVmg();});\n"
"$('#prt').onclick=()=>window.print();\n"
"function renderVmg(){if(!P||!P.curves||!P.curves.length){$('#vmgtable').innerHTML='';return;}\n"
" let h='<table class=dt><tr><th>TWS</th><th>'+T('vmgup')+' °</th><th>BS</th><th>VMG</th><th>'+T('vmgdn')+' °</th><th>BS</th><th>VMG</th></tr>';\n"
" P.curves.forEach((c,k)=>{const au=c.a_up,ad=c.a_dn,bu=interpBS(c,au),bd=interpBS(c,ad);\n"
"  const vu=Math.abs(bu*Math.cos(au*Math.PI/180)),vd=Math.abs(bd*Math.cos(ad*Math.PI/180));\n"
"  h+='<tr><td>'+P.tws[k]+'</td><td>'+au.toFixed(0)+'</td><td>'+bu.toFixed(2)+'</td><td>'+vu.toFixed(2)+'</td><td>'+ad.toFixed(0)+'</td><td>'+bd.toFixed(2)+'</td><td>'+vd.toFixed(2)+'</td></tr>';});\n"
"  $('#vmgtable').innerHTML=h+'</table>';}\n"
"function applyTheme(){document.body.classList.toggle('light',theme==='light');$('#theme').textContent=theme==='dark'?'☀':'🌙';}\n"
"$('#lang').onclick=()=>{lang=lang==='fr'?'en':'fr';localStorage.setItem('lang',lang);$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();load();};\n"
"$('#theme').onclick=()=>{theme=theme==='dark'?'light':'dark';localStorage.setItem('theme',theme);applyTheme();draw();};\n"
"applyTheme();$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();loadBoat();load();\n"
"pollLive().then(()=>{if(LIVE&&LIVE.on)startPoll();});\n"
"</script></body></html>\n";

/* ------------------------------------------------------------- HTTP utils --- */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) continue;
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

static int hexv(char c) { if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }

/* Décode un composant d'URL (%XX et +) jusqu'à '&' ou fin. */
static void url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && in[i] != '&' && o < cap - 1; i++) {
        if (in[i] == '%' && in[i+1] && in[i+2]) {
            int hi = hexv(in[i+1]), lo = hexv(in[i+2]);
            if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); i += 2; continue; }
        }
        out[o++] = (in[i] == '+') ? ' ' : in[i];
    }
    out[o] = 0;
}

static void send_resp(int fd, int code, const char *status, const char *ctype,
                      const char *body, size_t len)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, status, ctype, len);
    if (h > 0) { ssize_t w = write(fd, hdr, (size_t)h); (void)w; }
    if (body && len) { ssize_t w = write(fd, body, len); (void)w; }
}

static void send_text(int fd, int code, const char *status, const char *ctype, const char *body)
{ send_resp(fd, code, status, ctype, body, strlen(body)); }

/* --- auth HTTP Basic (repris de n2k-mux) --- */
static void b64encode(const char *in, char *out, size_t cap)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), i = 0, o = 0;
    while (i < n && o + 4 < cap) {
        unsigned v = (unsigned char)in[i++] << 16; int rem = 1;
        if (i < n) { v |= (unsigned char)in[i++] << 8; rem = 2; }
        if (i < n) { v |= (unsigned char)in[i++];      rem = 3; }
        out[o++] = A[(v >> 18) & 63]; out[o++] = A[(v >> 12) & 63];
        out[o++] = rem >= 2 ? A[(v >> 6) & 63] : '='; out[o++] = rem >= 3 ? A[v & 63] : '=';
    }
    out[o] = '\0';
}

static int ct_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

static void send_401(int fd)
{
    static const char *r =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"polar_doctor\", charset=\"UTF-8\"\r\n"
        "Content-Type: text/plain\r\nContent-Length: 16\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n401 Unauthorized";
    ssize_t w = write(fd, r, strlen(r)); (void)w;
}

static int authed(const char *req)
{
    if (!g_auth) return 1;
    const char *h = strcasestr(req, "Authorization:");
    if (!h) return 0;
    const char *b = strcasestr(h, "Basic ");
    if (!b) return 0;
    b += 6;
    char tok[sizeof g_auth_b64]; size_t i = 0;
    while (b[i] && b[i] != '\r' && b[i] != '\n' && b[i] != ' ' && i < sizeof tok - 1) { tok[i] = b[i]; i++; }
    tok[i] = '\0';
    return ct_eq(tok, g_auth_b64);
}

/* ----------------------------------------------------------- API /polar --- */
/* Émet "tws":[...],"curves":[...] de pd (échantillonnage = draw_tws_curve GTK :
 * rangées présentes + frontières VMG a_up/a_dn interpolées). Partagé /api/polar
 * et la polaire vivante de /api/live. false = débordement de buffer. */
static bool append_polar_curves(char *buf, size_t cap, size_t *pn, PolarData *pd)
{
    size_t n = *pn; int w;
#define APPC(...) do { w = snprintf(buf + n, cap - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= cap - n) return false; n += (size_t)w; } while (0)
    int keep[MAX_SPEEDS], nk = 0;
    for (int s = 0; s < pd->num_speeds; s++)
        if (pd->tws_values[s] != 0) keep[nk++] = s;
    APPC("\"tws\":[");
    for (int k = 0; k < nk; k++) APPC("%s%d", k ? "," : "", pd->tws_values[keep[k]]);
    APPC("],\"curves\":[");
    for (int k = 0; k < nk; k++) {
        double tws = pd->tws_values[keep[k]], a_up, a_dn;
        vmg_optimal_angles(pd, tws, &a_up, &a_dn);
        double angs[MAX_ANGLES + 2]; int m = 0;
        for (int i = 0; i < pd->num_angles; i++)
            if (pd->twa_present[i]) angs[m++] = pd->twa_values[i];
        angs[m++] = a_up; angs[m++] = a_dn;
        for (int i = 0; i < m - 1; i++)
            for (int j = i + 1; j < m; j++)
                if (angs[i] > angs[j]) { double t = angs[i]; angs[i] = angs[j]; angs[j] = t; }
        APPC("%s{\"tws\":%d,\"a_up\":%.1f,\"a_dn\":%.1f,\"pts\":[",
             k ? "," : "", pd->tws_values[keep[k]], a_up, a_dn);
        int np = 0; double lastang = -1;
        for (int i = 0; i < m; i++) {
            if (np > 0 && fabs(angs[i] - lastang) < 1e-6) continue;
            double bsp = interpolate_polar_bsp(pd, angs[i], tws);
            if (bsp < 0.01) continue;
            APPC("%s[%.1f,%.2f]", np ? "," : "", angs[i], bsp);
            lastang = angs[i]; np++;
        }
        APPC("]}");
    }
    APPC("]");
#undef APPC
    *pn = n;
    return true;
}

static void serve_polar(int fd)
{
    if (!g_loaded) { send_text(fd, 200, "OK", "application/json", "{\"filename\":\"\",\"tws\":[],\"twa\":[],\"bsp\":[],\"curves\":[]}"); return; }

    int keep[MAX_SPEEDS], nk = 0;
    for (int s = 0; s < g_polar.num_speeds; s++)
        if (g_polar.tws_values[s] != 0) keep[nk++] = s;

    static char buf[JSON_MAX];
    size_t n = 0; int w;
#define APP(...) do { w = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    n += (size_t)w; } while (0)
    char e[300];
    json_escape(g_polar.filename, e, sizeof e);
    APP("{\"filename\":\"%s\",\"twa\":[", e);
    for (int a = 0; a < g_polar.num_angles; a++) APP("%s%d", a ? "," : "", g_polar.twa_values[a]);
    APP("],\"bsp\":[");
    for (int a = 0; a < g_polar.num_angles; a++) {
        APP("%s[", a ? "," : "");
        for (int k = 0; k < nk; k++) APP("%s%.2f", k ? "," : "", g_polar.polar_data[a][keep[k]]);
        APP("]");
    }
    APP("],");
    if (!append_polar_curves(buf, sizeof buf, &n, &g_polar)) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
    {
        double pt = 0, pa = 0, pm = polar_absolute_max(&g_polar, &pt, &pa);
        APP(",\"pmax\":%.2f,\"pmax_tws\":%.1f,\"pmax_twa\":%.1f}", pm, pt, pa);
    }
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* --- Bateau : liste des .pol d'un dossier --- */
static void base_no_ext(const char *path, char *out, size_t cap)
{
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    snprintf(out, cap, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".pol") == 0) *dot = '\0';
}

static void scan_boat(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_npol < MAXPOL) {
        size_t l = strlen(e->d_name);
        if (l < 4 || strcasecmp(e->d_name + l - 4, ".pol") != 0) continue;
        snprintf(g_pol_paths[g_npol], sizeof g_pol_paths[0], "%s/%s", dir, e->d_name);
        base_no_ext(e->d_name, g_pol_names[g_npol], sizeof g_pol_names[0]);
        g_npol++;
    }
    closedir(d);
    for (int i = 0; i < g_npol - 1; i++)            /* tri alphabétique */
        for (int j = i + 1; j < g_npol; j++)
            if (strcasecmp(g_pol_names[i], g_pol_names[j]) > 0) {
                char tp[512], tn[128];
                snprintf(tp, sizeof tp, "%s", g_pol_paths[i]); snprintf(tn, sizeof tn, "%s", g_pol_names[i]);
                snprintf(g_pol_paths[i], 512, "%s", g_pol_paths[j]); snprintf(g_pol_names[i], 128, "%s", g_pol_names[j]);
                snprintf(g_pol_paths[j], 512, "%s", tp); snprintf(g_pol_names[j], 128, "%s", tn);
            }
}

/* Re-scanne le dossier-bateau en conservant la polaire courante (par nom). */
static void rescan_boat(void)
{
    if (!g_boat_dir[0]) return;
    char cur[128] = "";
    if (g_cur >= 0 && g_cur < g_npol) snprintf(cur, sizeof cur, "%s", g_pol_names[g_cur]);
    g_npol = 0; scan_boat(g_boat_dir);
    g_cur = 0;
    for (int i = 0; i < g_npol; i++) if (strcmp(g_pol_names[i], cur) == 0) { g_cur = i; break; }
}

/* GET /api/boat : nom du bateau + liste des polaires + index courant. */
static void serve_boat(int fd)
{
    static char buf[8192]; size_t n = 0; int w;
#define APP(...) do { w = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    n += (size_t)w; } while (0)
    char e[160]; json_escape(g_boat_name, e, sizeof e);
    APP("{\"name\":\"%s\",\"current\":%d,\"polars\":[", e, g_cur);
    for (int i = 0; i < g_npol; i++) { json_escape(g_pol_names[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); }
    APP("],\"mains\":[");
    for (int i = 0; i < g_boat_config.n_mainsail; i++) { json_escape(g_boat_config.mainsail[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); }
    APP("],\"heads\":[");
    for (int i = 0; i < g_boat_config.n_headsail; i++) { json_escape(g_boat_config.headsail[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); }
    APP("],\"seas\":[");
    for (int i = 0; i < g_boat_config.n_seastate; i++) { json_escape(g_boat_config.seastate[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); }
    { char m[80], h[80], s[80];
      json_escape(g_cur_main, m, sizeof m); json_escape(g_cur_head, h, sizeof h); json_escape(g_cur_sea, s, sizeof s);
      APP("],\"cur_main\":\"%s\",\"cur_head\":\"%s\",\"cur_sea\":\"%s\"}", m, h, s); }
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* GET /api/select?i=N : charge la polaire N comme polaire courante. */
static void serve_select(int fd, int i)
{
    if (i < 0 || i >= g_npol) { send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}"); return; }
    PolarData tmp; init_polar_data(&tmp);
    if (load_polar_file(g_pol_paths[i], &tmp)) {
        g_polar = tmp; g_loaded = 1; g_cur = i;
        g_disp = def_index_for_selected();   /* la polaire affichée en live suit la sélection */
        send_text(fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else send_text(fd, 500, "Error", "application/json", "{\"ok\":false}");
}

/* GET /api/curve?tws=X : une courbe interpolée pour une TWS quelconque (mode dynamique). */
static void serve_curve(int fd, double tws)
{
    if (!g_loaded || tws <= 0) { send_text(fd, 200, "OK", "application/json", "{\"tws\":0,\"a_up\":0,\"a_dn\":0,\"pts\":[]}"); return; }
    double a_up, a_dn;
    vmg_optimal_angles(&g_polar, tws, &a_up, &a_dn);
    double angs[MAX_ANGLES + 2]; int m = 0;
    for (int i = 0; i < g_polar.num_angles; i++)
        if (g_polar.twa_present[i]) angs[m++] = g_polar.twa_values[i];
    angs[m++] = a_up; angs[m++] = a_dn;
    for (int i = 0; i < m - 1; i++)
        for (int j = i + 1; j < m; j++)
            if (angs[i] > angs[j]) { double t = angs[i]; angs[i] = angs[j]; angs[j] = t; }
    static char buf[8192];
    size_t n = 0; int w;
#define APP(...) do { w = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    n += (size_t)w; } while (0)
    APP("{\"tws\":%.2f,\"a_up\":%.1f,\"a_dn\":%.1f,\"pts\":[", tws, a_up, a_dn);
    int np = 0; double lastang = -1, cmax = 0, cmaxa = 0;
    for (int i = 0; i < m; i++) {
        if (np > 0 && fabs(angs[i] - lastang) < 1e-6) continue;
        double bsp = interpolate_polar_bsp(&g_polar, angs[i], tws);
        if (bsp < 0.01) continue;
        APP("%s[%.1f,%.2f]", np ? "," : "", angs[i], bsp);
        if (bsp > cmax) { cmax = bsp; cmaxa = angs[i]; }
        lastang = angs[i]; np++;
    }
    APP("],\"cmax\":%.2f,\"cmax_twa\":%.1f}", cmax, cmaxa);
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* ============================ Capture live (P1) ============================ *
 * Un seul process, intégré à la boucle poll() du serveur. Réutilise le pipeline
 * d'import.c (parse_nmea_sentence, débruitage STW/SOG, lissage, grille).
 * Sources : NMEA TCP (client) et NMEA UDP (écoute). État exposé en polling. */

static int    g_live_on = 0, g_live_src = 0, g_live_fd = -1;  /* src 1=tcp 2=udp 3=vdr */
static int    g_live_moteur = 0;           /* moteur embrayé -> on ignore les points */
static char   g_live_addr[128] = "";
static long   g_live_count = 0;
static double g_cur_twa = -1, g_cur_bsp = 0, g_cur_tws = 0;
#define LIVE_PTS 1000
static float  g_lpt[LIVE_PTS][2];          /* tampon circulaire (twa,bsp) pour le nuage */
static int    g_lpt_n = 0, g_lpt_head = 0;
static nmea_data_t    g_lnmea;
static nmea_smoother_t g_lsm;
static stw_sog_filter_t g_lfilt;
static char   g_acc[4096]; static size_t g_acclen = 0;  /* accumulateur de ligne */
static sqlite3 *g_vdr = NULL;              /* source VDR (tail) */
static sqlite3_int64 g_vdr_last = 0;
static int    g_vdr_has_sog = 0;
static char   g_live_saved[600] = "";      /* chemin du .pol écrit au dernier arrêt */

/* Index de la définition de polaire (boat.cfg) correspondant à la polaire
 * sélectionnée, par nom de fichier. 0 par défaut. */
static int def_index_for_selected(void)
{
    if (g_routing && g_cur >= 0 && g_cur < g_npol)
        for (int k = 0; k < g_boat_config.n_polars; k++)
            if (strcmp(g_boat_config.polars[k].name, g_pol_names[g_cur]) == 0) return k;
    return 0;
}

static void live_reset(void)
{
    memset(&g_lnmea, 0, sizeof g_lnmea);
    nmea_smoother_reset(&g_lsm); stw_sog_reset(&g_lfilt);
    g_lpt_n = g_lpt_head = 0; g_live_count = 0;
    g_cur_twa = -1; g_cur_bsp = g_cur_tws = 0; g_acclen = 0;
    g_live_saved[0] = 0;
    for (int k = 0; k < BOAT_MAX_POLARS; k++) g_gadd[k] = 0;
}

/* Range un point : routage multi-polaires si actif (toutes les polaires dont les
 * critères matchent l'état courant), sinon grille unique. Le nuage et le compteur
 * reflètent la polaire AFFICHÉE (g_disp). Ignoré si moteur embrayé. */
static void live_add(double twa, double tws, double bsp)
{
    if (g_live_moteur) return;
    int disp_hit = 0;
    if (g_routing) {
        for (int k = 0; k < g_ng; k++)
            if (polar_def_matches(&g_boat_config.polars[k], g_cur_main, g_cur_head, g_cur_sea)) {
                add_data_point(&g_grids[k], twa, tws, bsp); g_gadd[k]++;
                if (k == g_disp) disp_hit = 1;
            }
    } else {
        add_data_point(&g_grids[0], twa, tws, bsp); g_gadd[0]++; disp_hit = 1;
    }
    if (!disp_hit) return;
    g_lpt[g_lpt_head][0] = (float)twa; g_lpt[g_lpt_head][1] = (float)bsp;
    g_lpt_head = (g_lpt_head + 1) % LIVE_PTS; if (g_lpt_n < LIVE_PTS) g_lpt_n++;
    g_cur_twa = twa; g_cur_bsp = bsp; g_cur_tws = tws; g_live_count++;
}

/* Une phrase NMEA complète : même pipeline que process_nmea_file (lissé). */
static void live_feed_sentence(const char *line)
{
    if (!parse_nmea_sentence(line, &g_lnmea)) return;
    if (g_lnmea.has_sog && !stw_sog_accept(&g_lfilt, g_lnmea.bsp, g_lnmea.sog)) return;
    double twa = g_lnmea.twa, tws = g_lnmea.tws, bsp = g_lnmea.bsp;
    if (NMEA_SMOOTH_WINDOW > 1)
        nmea_smoother_push(&g_lsm, g_lnmea.twa, g_lnmea.tws, g_lnmea.bsp, &twa, &tws, &bsp);
    live_add(twa, tws, bsp);
}

/* Tick VDR : ingère les lignes ajoutées depuis le dernier TIME vu (non lissé). */
static void live_vdr_tick(void)
{
    if (!g_vdr) return;
    const char *sql = g_vdr_has_sog
        ? "SELECT TIME,TWA,TWS,STW,SOG FROM VDR WHERE TIME>? ORDER BY TIME"
        : "SELECT TIME,TWA,TWS,STW FROM VDR WHERE TIME>? ORDER BY TIME";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_vdr, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, g_vdr_last);
    while (sqlite3_step(st) == SQLITE_ROW) {
        g_vdr_last = sqlite3_column_int64(st, 0);
        double twa = sqlite3_column_double(st, 1), tws = sqlite3_column_double(st, 2),
               stw = sqlite3_column_double(st, 3);
        if (g_vdr_has_sog) {
            double sog = sqlite3_column_double(st, 4);
            if (sog > 0 && !stw_sog_accept(&g_lfilt, stw, sog)) continue;
        }
        live_add(twa, tws, stw);
    }
    sqlite3_finalize(st);
}

/* Ouvre le VDR en lecture seule et se positionne après la dernière ligne (live). */
static int live_vdr_open(const char *path)
{
    if (sqlite3_open_v2(path, &g_vdr, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (g_vdr) { sqlite3_close(g_vdr); g_vdr = NULL; }
        return -1;
    }
    g_vdr_has_sog = vdr_has_column(g_vdr, "SOG");
    g_vdr_last = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_vdr, "SELECT MAX(TIME) FROM VDR", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) g_vdr_last = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return 0;
}

/* Découpe un flux (TCP) ou datagramme (UDP) en lignes via l'accumulateur. */
static void live_feed(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (g_acclen) { g_acc[g_acclen] = 0; live_feed_sentence(g_acc); g_acclen = 0; }
        } else if (g_acclen < sizeof g_acc - 1) g_acc[g_acclen++] = c;
    }
}

static int open_tcp(const char *addr)
{
    char host[128] = "127.0.0.1", port[16] = "10110";
    const char *c = strrchr(addr, ':');
    if (c) { size_t hl = (size_t)(c - addr); if (hl && hl < sizeof host) { memcpy(host, addr, hl); host[hl] = 0; } snprintf(port, sizeof port, "%s", c + 1); }
    else snprintf(port, sizeof port, "%s", addr);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
    return fd;
}

static int open_udp(const char *addr)
{
    const char *c = strrchr(addr, ':');
    int port = atoi(c ? c + 1 : addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

static void live_stop(void)
{
    if (g_live_fd >= 0) close(g_live_fd);
    g_live_fd = -1;
    if (g_vdr) { sqlite3_close(g_vdr); g_vdr = NULL; }
    g_live_on = 0;
}

static void live_start(int src, const char *addr)
{
    live_stop(); live_reset();
    /* (Re)configuration des grilles : une par polaire si le bateau en définit
     * (routage), ensemencées depuis la polaire existante (base + live) ; sinon
     * une grille unique repartant de zéro. */
    for (int k = 0; k < g_ng; k++) free_polar_grid(&g_grids[k]);
    g_routing = (g_boat_config.n_polars > 0 && g_boat_dir[0]);
    g_ng = g_routing ? g_boat_config.n_polars : 1;
    if (g_ng > BOAT_MAX_POLARS) g_ng = BOAT_MAX_POLARS;
    for (int k = 0; k < g_ng; k++) {
        init_polar_grid(&g_grids[k]);
        if (g_routing) {
            char pth[700]; snprintf(pth, sizeof pth, "%s/%s.pol", g_boat_dir, g_boat_config.polars[k].name);
            load_existing_polar_for_update(pth, &g_grids[k], NULL);
        }
    }
    g_disp = def_index_for_selected();

    int ok;
    if (src == 3) ok = (live_vdr_open(addr) == 0);
    else { int fd = (src == 1) ? open_tcp(addr) : open_udp(addr); if (fd >= 0) g_live_fd = fd; ok = (fd >= 0); }
    if (ok) { g_live_on = 1; g_live_src = src; snprintf(g_live_addr, sizeof g_live_addr, "%s", addr); }
}

/* Agrège la grille live et l'enregistre en .pol horodaté dans le dossier du
 * bateau (ou le dossier de la polaire courante). Appelé à l'arrêt explicite. */
static void live_save(void)
{
    g_live_saved[0] = 0;
    static double res[PG_MAX_ANGLES][PG_MAX_SPEEDS];
    static PolarData lp;
    if (g_routing) {
        /* Une polaire par définition mise à jour (base ensemencée + live) → <nom>.pol. */
        char names[512] = ""; int saved = 0;
        for (int k = 0; k < g_ng; k++) {
            if (g_gadd[k] <= 0) continue;
            compute_polar(&g_grids[k], res, NULL);
            load_polar_from_grid(&lp, &g_grids[k], res);
            char path[700]; snprintf(path, sizeof path, "%s/%s.pol", g_boat_dir, g_boat_config.polars[k].name);
            if (save_polar_file(path, &lp)) {
                size_t l = strlen(names);
                snprintf(names + l, sizeof names - l, "%s%s.pol", l ? ", " : "", g_boat_config.polars[k].name);
                saved++;
            }
        }
        if (saved) snprintf(g_live_saved, sizeof g_live_saved, "%s", names);
    } else {
        if (g_gadd[0] <= 0) return;
        compute_polar(&g_grids[0], res, NULL);
        load_polar_from_grid(&lp, &g_grids[0], res);
        char dir[512] = ".";
        if (g_npol > 0) {
            int i = (g_cur >= 0 && g_cur < g_npol) ? g_cur : 0;
            snprintf(dir, sizeof dir, "%s", g_pol_paths[i]);
            char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
        }
        time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
        char ts[32]; strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);
        char path[700]; snprintf(path, sizeof path, "%s/live_%s.pol", dir, ts);
        if (save_polar_file(path, &lp)) snprintf(g_live_saved, sizeof g_live_saved, "%s", path);
    }
}

/* GET /api/live : état + nuage de points + point courant (polling). */
static void serve_live(int fd)
{
    static char buf[JSON_MAX]; size_t n = 0; int w;
#define APP(...) do { w = snprintf(buf + n, sizeof buf - n, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - n) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    n += (size_t)w; } while (0)
    APP("{\"on\":%s,\"src\":%d,\"count\":%ld,", g_live_on ? "true" : "false", g_live_src, g_live_count);
    if (g_cur_twa >= 0 && g_cur_bsp > 0) APP("\"cur\":[%.1f,%.2f,%.1f],", g_cur_twa, g_cur_bsp, g_cur_tws);
    else APP("\"cur\":null,");
    APP("\"pts\":[");
    int start = (g_lpt_head - g_lpt_n + LIVE_PTS) % LIVE_PTS;
    for (int k = 0; k < g_lpt_n; k++) {
        int idx = (start + k) % LIVE_PTS;
        APP("%s[%.1f,%.2f]", k ? "," : "", g_lpt[idx][0], g_lpt[idx][1]);
    }
    { char e[700]; json_escape(g_live_saved, e, sizeof e);
      APP("],\"moteur\":%s,\"saved\":\"%s\",", g_live_moteur ? "true" : "false", e); }
    /* Polaire vivante : agrège la grille live (P90, min 3 pts) → courbes en cours. */
    {
        static double res[PG_MAX_ANGLES][PG_MAX_SPEEDS];
        static PolarData lp;
        compute_polar(&g_grids[g_disp], res, NULL);
        load_polar_from_grid(&lp, &g_grids[g_disp], res);
        if (!append_polar_curves(buf, sizeof buf, &n, &lp)) { send_text(fd, 500, "Error", "application/json", "{}"); return; }
    }
    APP("}");
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* POST /api/save : corps = texte .pol (édité côté navigateur). Écrit dans un .tmp,
 * valide en le rechargeant, puis remplace la polaire courante atomiquement. */
static void serve_save(int fd, char *body)
{
    if (g_npol <= 0 || g_cur < 0 || g_cur >= g_npol) { send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}"); return; }
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pol_paths[g_cur]);
    int fdw = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdw < 0) { send_text(fd, 500, "Error", "application/json", "{\"ok\":false}"); return; }
    size_t len = strlen(body), w = 0;
    while (w < len) { ssize_t x = write(fdw, body + w, len - w); if (x <= 0) break; w += (size_t)x; }
    close(fdw);
    PolarData test; init_polar_data(&test);
    /* load_polar_file est laxiste : on exige une polaire non dégénérée (>=1 TWA,
     * >=2 colonnes TWS dont la sentinelle 0) avant de remplacer le fichier. */
    if (load_polar_file(tmp, &test) && test.num_angles >= 1 && test.num_speeds >= 2
        && rename(tmp, g_pol_paths[g_cur]) == 0) {
        g_polar = test;
        snprintf(g_polar.filename, sizeof g_polar.filename, "%s", g_pol_paths[g_cur]);
        g_loaded = 1;
        send_text(fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else {
        unlink(tmp);
        send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}");
    }
}

/* POST /api/import?mode=create|update : corps = chemins de fichiers NMEA/VDR
 * (un par ligne, locaux au serveur). create → nouvelle polaire import_<ts>.pol ;
 * update → réagrège l'existant (sélectionné) + les fichiers, écrit sur place. */
static void serve_import(int fd, char *body, int update)
{
    polar_grid_t g; init_polar_grid(&g);
    if (update && g_npol > 0 && g_cur >= 0 && g_cur < g_npol)
        load_existing_polar_for_update(g_pol_paths[g_cur], &g, NULL);

    int total = 0, files = 0;
    char *sp = NULL;
    for (char *line = strtok_r(body, "\r\n", &sp); line; line = strtok_r(NULL, "\r\n", &sp)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        int c = process_file(p, &g, NULL);
        if (c >= 0) { total += c; files++; }
    }

    static double res[PG_MAX_ANGLES][PG_MAX_SPEEDS];
    static PolarData lp;
    compute_polar(&g, res, NULL);
    load_polar_from_grid(&lp, &g, res);

    char path[700];
    if (update && g_npol > 0 && g_cur >= 0 && g_cur < g_npol) {
        snprintf(path, sizeof path, "%s", g_pol_paths[g_cur]);
    } else {
        char dir[512] = ".";
        if (g_boat_dir[0]) snprintf(dir, sizeof dir, "%s", g_boat_dir);
        else if (g_npol > 0) { snprintf(dir, sizeof dir, "%s", g_pol_paths[0]); char *s = strrchr(dir, '/'); if (s) *s = 0; else snprintf(dir, sizeof dir, "."); }
        time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
        char ts[32]; strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);
        snprintf(path, sizeof path, "%s/import_%s.pol", dir, ts);
    }

    int ok = (lp.num_angles >= 1 && lp.num_speeds >= 2 && save_polar_file(path, &lp));
    if (ok) {
        PolarData t; init_polar_data(&t);
        if (load_polar_file(path, &t)) { g_polar = t; snprintf(g_polar.filename, sizeof g_polar.filename, "%s", path); g_loaded = 1; }
        rescan_boat();
        for (int i = 0; i < g_npol; i++) if (strcmp(g_pol_paths[i], path) == 0) { g_cur = i; break; }
    }
    free_polar_grid(&g);

    char e[700], out[900]; json_escape(ok ? path : "", e, sizeof e);
    snprintf(out, sizeof out, "{\"ok\":%s,\"files\":%d,\"points\":%d,\"saved\":\"%s\"}",
             ok ? "true" : "false", files, total, e);
    send_text(fd, ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", out);
}

/* --------------------------------------------------------------- client --- */
static void handle_client(int fd)
{
    static char req[REQ_MAX];
    size_t n = 0; ssize_t r;
    while (n < sizeof req - 1) {
        r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r; req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (n == 0) return;
    req[n] = '\0';

    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);

    if (!authed(req)) { send_401(fd); return; }

    /* Corps (POST) : complète la lecture selon Content-Length. */
    char *hdr_end = strstr(req, "\r\n\r\n");
    char *body = hdr_end ? hdr_end + 4 : NULL;
    if (body) {
        const char *cl = strcasestr(req, "Content-Length:");
        if (cl) {
            size_t want = (size_t)strtoul(cl + 15, NULL, 10);
            size_t have = n - (size_t)(body - req);
            while (have < want && n < sizeof req - 1) {
                r = recv(fd, req + n, sizeof req - 1 - n, 0);
                if (r <= 0) break;
                n += (size_t)r; have += (size_t)r;
            }
            req[n] = '\0';
            body = strstr(req, "\r\n\r\n") + 4;
            body[want < have ? want : have] = '\0';
        }
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/save") == 0 && body) serve_save(fd, body);
        else if (strncmp(path, "/api/import", 11) == 0 && body)
            serve_import(fd, body, strstr(path, "update") != NULL);
        else send_text(fd, 404, "Not Found", "text/plain", "404\n");
        return;
    }
    if (strcmp(method, "GET") != 0) { send_text(fd, 400, "Bad Request", "text/plain", "400\n"); return; }

    if (strcmp(path, "/") == 0)
        send_resp(fd, 200, "OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
    else if (strcmp(path, "/api/polar") == 0)
        serve_polar(fd);
    else if (strncmp(path, "/api/curve", 10) == 0) {
        const char *q = strstr(path, "tws=");
        serve_curve(fd, q ? atof(q + 4) : 0);
    }
    else if (strcmp(path, "/api/boat") == 0)
        serve_boat(fd);
    else if (strncmp(path, "/api/select", 11) == 0) {
        const char *q = strstr(path, "i=");
        serve_select(fd, q ? atoi(q + 2) : -1);
    }
    else if (strncmp(path, "/api/live/start", 15) == 0) {
        const char *ps = strstr(path, "src="), *pa = strstr(path, "addr=");
        int src = 2;
        if (ps) { if (strncmp(ps + 4, "tcp", 3) == 0) src = 1; else if (strncmp(ps + 4, "vdr", 3) == 0) src = 3; }
        char addr[128] = "10110";
        if (pa) { pa += 5; size_t i = 0; while (pa[i] && pa[i] != '&' && i < sizeof addr - 1) { addr[i] = pa[i]; i++; } addr[i] = 0; }
        live_start(src, addr);
        serve_live(fd);
    }
    else if (strcmp(path, "/api/live/stop") == 0) { live_save(); live_stop(); rescan_boat(); serve_live(fd); }
    else if (strncmp(path, "/api/live/state", 15) == 0) {
        const char *pm = strstr(path, "main="), *ph = strstr(path, "head="), *ps = strstr(path, "sea=");
        if (pm) url_decode(pm + 5, g_cur_main, sizeof g_cur_main); else g_cur_main[0] = 0;
        if (ph) url_decode(ph + 5, g_cur_head, sizeof g_cur_head); else g_cur_head[0] = 0;
        if (ps) url_decode(ps + 4, g_cur_sea, sizeof g_cur_sea); else g_cur_sea[0] = 0;
        serve_live(fd);
    }
    else if (strncmp(path, "/api/live/moteur", 16) == 0) {
        const char *q = strstr(path, "on=");
        g_live_moteur = (q && q[3] == '1') ? 1 : 0;
        serve_live(fd);
    }
    else if (strcmp(path, "/api/live") == 0) serve_live(fd);
    else
        send_text(fd, 404, "Not Found", "text/plain", "404\n");
}

int main(int argc, char **argv)
{
    int port = 8081;   /* 8080 est pris par n2k-mux-web */
    const char *bind_addr = "127.0.0.1";
    const char *pol = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_addr = argv[++i];
        else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) g_auth = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "Usage : %s [fichier.pol | dossier-bateau] [--port N] [--bind ADDR] [--auth user:pass]\n"
                "  --port N     port d'écoute (défaut 8081 ; 8080 = n2k-mux-web)\n"
                "  --bind ADDR  adresse d'écoute (défaut 127.0.0.1 ; 0.0.0.0 = LAN)\n"
                "  --auth u:p   authentification HTTP Basic\n", argv[0]);
            return 0;
        } else if (argv[i][0] != '-') pol = argv[i];
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); return 2; }
    }

    init_polar_data(&g_polar);
    struct stat st;
    if (pol && stat(pol, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* dossier-bateau : lister les .pol, charger le premier */
        char dir[512]; snprintf(dir, sizeof dir, "%s", pol);
        size_t L = strlen(dir); while (L > 1 && dir[L - 1] == '/') dir[--L] = '\0';
        const char *b = strrchr(dir, '/'); snprintf(g_boat_name, sizeof g_boat_name, "%s", b ? b + 1 : dir);
        char cfg[BOAT_PATH_LEN];                 /* nom réel depuis boat.cfg si présent */
        if (boat_find_config(dir, cfg, sizeof cfg) && boat_config_load(&g_boat_config, cfg) && g_boat_config.name[0])
            snprintf(g_boat_name, sizeof g_boat_name, "%s", g_boat_config.name);
        snprintf(g_boat_dir, sizeof g_boat_dir, "%s", dir);
        scan_boat(dir);
        if (g_npol > 0 && load_polar_file(g_pol_paths[0], &g_polar)) { g_loaded = 1; g_cur = 0; }
        else fprintf(stderr, "polar_doctor_web : aucune polaire chargeable dans %s\n", dir);
    } else if (pol) {
        if (load_polar_file(pol, &g_polar)) g_loaded = 1;
        else fprintf(stderr, "polar_doctor_web : impossible de charger %s\n", pol);
        snprintf(g_pol_paths[0], sizeof g_pol_paths[0], "%s", pol);
        base_no_ext(pol, g_pol_names[0], sizeof g_pol_names[0]);
        g_npol = 1; g_cur = 0;
    }

    if (g_auth) {
        if (!strchr(g_auth, ':')) { fprintf(stderr, "--auth attend user:pass\n"); return 2; }
        b64encode(g_auth, g_auth_b64, sizeof g_auth_b64);
    }

    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { fprintf(stderr, "adresse invalide : %s\n", bind_addr); return 2; }
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
    if (listen(ls, 8) != 0) { perror("listen"); return 1; }

    fprintf(stderr, "polar_doctor_web : http://%s:%d/  (polaire : %s, auth : %s)\n",
            bind_addr, port, g_loaded ? g_polar.filename : "(aucune)", g_auth ? "oui" : "non");

    init_polar_grid(&g_grids[0]);
    for (;;) {
        struct pollfd pfds[2];
        int nf = 0;
        pfds[nf].fd = ls; pfds[nf].events = POLLIN; nf++;
        if (g_live_on && g_live_fd >= 0) { pfds[nf].fd = g_live_fd; pfds[nf].events = POLLIN; nf++; }
        int r = poll(pfds, nf, g_live_on ? 1000 : -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (pfds[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL);
            if (fd >= 0) {
                struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                handle_client(fd);
                close(fd);
            }
        }
        if (g_live_on && g_live_fd >= 0 && nf > 1 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char b[4096];
            ssize_t got = recv(g_live_fd, b, sizeof b, 0);
            if (got > 0) live_feed(b, (size_t)got);
            else if (got == 0 && g_live_src == 1) live_stop();  /* TCP fermé par la passerelle */
        }
        if (g_live_on && g_live_src == 3) live_vdr_tick();      /* tail VDR (~1/s) */
    }
    close(ls);
    return 0;
}
