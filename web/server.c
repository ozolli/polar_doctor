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

#include "polar_doctor.h"

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX   16384
#define JSON_MAX  262144   /* polaire en JSON (worst case ~110 Ko) */

static PolarData    g_polar;
static int          g_loaded = 0;
static const char  *g_auth = NULL;     /* "user:pass" attendu (NULL = pas d'auth) */
static char         g_auth_b64[352];

/* Bateau = dossier de .pol (liste POSIX ; le boat.cfg viendra avec libpolar). */
#define MAXPOL 64
static char g_boat_name[128] = "";
static char g_pol_paths[MAXPOL][512];
static char g_pol_names[MAXPOL][128];
static int  g_npol = 0;
static int  g_cur  = 0;

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
"</style></head><body>\n"
"<header><b>Polar Doctor</b><small id='fn'></small>\n"
"<span style='margin-left:auto;display:flex;gap:.5em'>"
"<button id='lang' class='hbtn'></button><button id='theme' class='hbtn'></button></span></header>\n"
"<main>\n"
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
"<div class='card'><h3 data-i18n='legend'>Légende (TWS)</h3><div id='leg' class='lg'></div></div>\n"
"<div class='card'><small id='info'></small></div>\n"
"</aside></main>\n"
"<script>\n"
"let lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"let theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"const L={fr:{boat:'Bateau',range:'Plage TWS',from:'De',to:'à',legend:'Légende (TWS)',kn:'nœuds',empty:'Aucune polaire chargée.',max:'Vitesse max',dyn:'Mode dynamique',dyn_on:'Activer',tws1:'TWS'},\n"
"en:{boat:'Boat',range:'TWS range',from:'From',to:'to',legend:'Legend (TWS)',kn:'knots',empty:'No polar loaded.',max:'Max speed',dyn:'Dynamic mode',dyn_on:'Enable',tws1:'TWS'}};\n"
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
"let DYN=null,CUR=null,G={cx:0,cy:0,R:0,top:1};\n"
"function interpBS(c,twa){const p=c.pts;if(!p||!p.length)return 0;if(twa<=p[0][0])return p[0][1];if(twa>=p[p.length-1][0])return p[p.length-1][1];\n"
" for(let i=0;i<p.length-1;i++)if(twa>=p[i][0]&&twa<=p[i+1][0]){const f=(twa-p[i][0])/((p[i+1][0]-p[i][0])||1);return p[i][1]+f*(p[i+1][1]-p[i][1]);}return 0;}\n"
"function draw(){const c=$('#cv'),x=c.getContext('2d');const W=c.width=c.clientWidth,H=c.height=c.clientHeight;\n"
" const cs=getComputedStyle(document.body),bd=cs.getPropertyValue('--border'),mu=cs.getPropertyValue('--muted');\n"
" x.clearRect(0,0,W,H);if(!P||!P.tws.length){x.fillStyle=mu;x.fillText(T('empty'),20,30);return;}\n"
" const dyn=$('#dyn').checked&&DYN;\n"
" let items;if(dyn){items=[{c:DYN,color:'rgb(0,204,0)'}];}else{items=shownIdx().map(s=>({c:P.curves[s],color:col(s),s:s})).filter(o=>o.c);}\n"
" let mx=0;for(const it of items)for(const q of it.c.pts)mx=Math.max(mx,q[1]);if(mx<=0)mx=1;\n"
" const ring=2;const top=Math.max(ring,Math.ceil(mx/ring)*ring);\n"  /* cercles tous les 2 nœuds */
" const cx=W*0.14,cy=H*0.5,R=Math.min(H*0.46,W*0.82);G={cx:cx,cy:cy,R:R,top:top};\n"
" const px=(twa,bsp)=>[cx+R*bsp/top*Math.sin(twa*Math.PI/180),cy-R*bsp/top*Math.cos(twa*Math.PI/180)];\n"
" x.strokeStyle=bd;x.fillStyle=mu;x.font='12px system-ui';x.textAlign='left';\n"
" for(let r=ring;r<=top+0.001;r+=ring){x.beginPath();for(let t=0;t<=180;t+=2){const p=px(t,r);t===0?x.moveTo(p[0],p[1]):x.lineTo(p[0],p[1]);}x.stroke();const lp=px(0,r);x.fillText(r,lp[0]+3,lp[1]+3);}\n"
" for(let t=0;t<=180;t+=15){x.beginPath();x.moveTo(cx,cy);const p=px(t,top);x.lineTo(p[0],p[1]);x.stroke();const lp=px(t,top*1.06);x.fillText(t+'°',lp[0]-6,lp[1]);}\n"
" x.lineWidth=2;items.forEach(it=>{if(it.c.pts.length<2)return;const pts=it.c.pts.map(q=>{const xy=px(q[0],q[1]);return [xy[0],xy[1],q[0]];});drawCurve(x,pts,it.c.a_up,it.c.a_dn,it.color);});x.lineWidth=1;\n"
" if(dyn&&CUR){const bs=CUR.bs,tr=CUR.twa*Math.PI/180,tws=DYN.tws;\n"
"  const aws=Math.sqrt(bs*bs+tws*tws+2*bs*tws*Math.cos(tr)),awa=Math.atan2(tws*Math.sin(tr),bs+tws*Math.cos(tr))*180/Math.PI,vmg=bs*Math.cos(tr);\n"
"  const p=px(CUR.twa,bs);x.strokeStyle='#1f6feb';x.lineWidth=1.5;x.beginPath();x.moveTo(cx,cy);x.lineTo(p[0],p[1]);x.stroke();x.fillStyle='#1f6feb';x.beginPath();x.arc(p[0],p[1],3,0,7);x.fill();x.lineWidth=1;\n"
"  $('#read').innerHTML='TWA <b>'+CUR.twa+'°</b> · AWA '+awa.toFixed(0)+'° · AWS '+aws.toFixed(1)+' · BS <b>'+bs.toFixed(2)+'</b> · VMG '+vmg.toFixed(2);}\n"
" else if(dyn)$('#read').textContent='';\n"
" if(dyn)$('#leg').innerHTML='<div><span class=sw style=\"background:rgb(0,204,0)\"></span>'+DYN.tws+' '+T('kn')+'</div>';\n"
" else $('#leg').innerHTML=items.map(it=>'<div><span class=sw style=\"background:'+it.color+'\"></span>'+P.tws[it.s]+' '+T('kn')+'</div>').join('');\n"
" $('#info').textContent=T('max')+' : '+mx.toFixed(2)+' '+T('kn');\n"
"}\n"
"async function load(){try{const r=await fetch('/api/polar');P=await r.json();}catch(e){P=null;}\n"
" $('#fn').textContent=P&&P.filename?(' — '+P.filename):'';\n"
" if(P&&P.tws.length){opt($('#from'),P.tws,0);opt($('#to'),P.tws,P.tws.length-1);}draw();}\n"
"$('#from').onchange=draw;$('#to').onchange=draw;addEventListener('resize',draw);\n"
"async function loadBoat(){try{const b=await fetch('/api/boat').then(r=>r.json());\n"
" $('#bname').textContent=b.name||'';\n"
" $('#polsel').innerHTML=(b.polars||[]).map((p,i)=>'<option value='+i+(i===b.current?' selected':'')+'>'+p+'</option>').join('');\n"
" $('#polsel').style.display=(b.polars&&b.polars.length>1)?'':'none';}catch(e){}}\n"
"$('#polsel').onchange=async e=>{await fetch('/api/select?i='+e.target.value);await load();if($('#dyn').checked)loadDyn();};\n"
"async function loadDyn(){const v=parseFloat($('#dtws').value)||0;try{const r=await fetch('/api/curve?tws='+v);DYN=await r.json();}catch(e){DYN=null;}CUR=null;draw();}\n"
"$('#dyn').onchange=()=>{if($('#dyn').checked)loadDyn();else{DYN=null;CUR=null;draw();}};\n"
"$('#dtws').onchange=()=>{if($('#dyn').checked)loadDyn();};\n"
"$('#cv').addEventListener('mousemove',e=>{if(!($('#dyn').checked&&DYN))return;const r=e.target.getBoundingClientRect();\n"
" const dx=(e.clientX-r.left)-G.cx,dy=G.cy-(e.clientY-r.top);let twa=Math.atan2(dx,dy)*180/Math.PI;twa=Math.max(0,Math.min(180,Math.round(twa)));\n"
" CUR={twa:twa,bs:interpBS(DYN,twa)};draw();});\n"
"$('#cv').addEventListener('mouseleave',()=>{if(CUR){CUR=null;draw();}});\n"
"function applyTheme(){document.body.classList.toggle('light',theme==='light');$('#theme').textContent=theme==='dark'?'☀':'🌙';}\n"
"$('#lang').onclick=()=>{lang=lang==='fr'?'en':'fr';localStorage.setItem('lang',lang);$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();load();};\n"
"$('#theme').onclick=()=>{theme=theme==='dark'?'light':'dark';localStorage.setItem('theme',theme);applyTheme();draw();};\n"
"applyTheme();$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();loadBoat();load();\n"
"</script></body></html>\n";

/*
 * Stub inerte : polar_data.c contient load_polar_from_memory() (non utilisé par
 * le serveur) qui référence add_data_point(), défini dans import.c. On évite de
 * tirer import.c — et avec lui tout GTK (callbacks de progression) — en
 * fournissant ce stub. À RETIRER lorsque le cœur sera extrait en libpolar avec
 * un header dégraissé (sans <gtk/gtk.h>).
 */
void add_data_point(polar_grid_t *grid, double twa, double tws, double bsp)
{ (void)grid; (void)twa; (void)tws; (void)bsp; }

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
static void serve_polar(int fd)
{
    if (!g_loaded) { send_text(fd, 200, "OK", "application/json", "{\"filename\":\"\",\"tws\":[],\"twa\":[],\"bsp\":[],\"curves\":[]}"); return; }

    /* indices de colonnes TWS à exposer (on saute la colonne sentinelle TWS 0). */
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
    APP("{\"filename\":\"%s\",\"tws\":[", e);
    for (int k = 0; k < nk; k++) APP("%s%d", k ? "," : "", g_polar.tws_values[keep[k]]);
    APP("],\"twa\":[");
    for (int a = 0; a < g_polar.num_angles; a++) APP("%s%d", a ? "," : "", g_polar.twa_values[a]);
    APP("],\"bsp\":[");
    for (int a = 0; a < g_polar.num_angles; a++) {
        APP("%s[", a ? "," : "");
        for (int k = 0; k < nk; k++) APP("%s%.2f", k ? "," : "", g_polar.polar_data[a][keep[k]]);
        APP("]");
    }
    /* Courbes échantillonnées comme draw_tws_curve (GTK) : rangées présentes +
     * points-frontières VMG (a_up, a_dn) interpolés → split couleur exact côté JS. */
    APP("],\"curves\":[");
    for (int k = 0; k < nk; k++) {
        double tws = g_polar.tws_values[keep[k]];
        double a_up, a_dn;
        vmg_optimal_angles(&g_polar, tws, &a_up, &a_dn);
        double angs[MAX_ANGLES + 2]; int m = 0;
        for (int i = 0; i < g_polar.num_angles; i++)
            if (g_polar.twa_present[i]) angs[m++] = g_polar.twa_values[i];
        angs[m++] = a_up; angs[m++] = a_dn;
        for (int i = 0; i < m - 1; i++)
            for (int j = i + 1; j < m; j++)
                if (angs[i] > angs[j]) { double t = angs[i]; angs[i] = angs[j]; angs[j] = t; }
        APP("%s{\"tws\":%d,\"a_up\":%.1f,\"a_dn\":%.1f,\"pts\":[",
            k ? "," : "", g_polar.tws_values[keep[k]], a_up, a_dn);
        int np = 0; double lastang = -1;
        for (int i = 0; i < m; i++) {
            if (np > 0 && fabs(angs[i] - lastang) < 1e-6) continue;
            double bsp = interpolate_polar_bsp(&g_polar, angs[i], tws);
            if (bsp < 0.01) continue;
            APP("%s[%.1f,%.2f]", np ? "," : "", angs[i], bsp);
            lastang = angs[i]; np++;
        }
        APP("]}");
    }
    {
        double pt = 0, pa = 0, pm = polar_absolute_max(&g_polar, &pt, &pa);
        APP("],\"pmax\":%.2f,\"pmax_tws\":%.1f,\"pmax_twa\":%.1f}", pm, pt, pa);
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
    APP("]}");
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

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        handle_client(fd);
        close(fd);
    }
    close(ls);
    return 0;
}
