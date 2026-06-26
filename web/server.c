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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define REQ_MAX   16384
#define JSON_MAX  262144   /* polaire en JSON (worst case ~110 Ko) */

static PolarData    g_polar;
static int          g_loaded = 0;
static const char  *g_auth = NULL;     /* "user:pass" attendu (NULL = pas d'auth) */
static char         g_auth_b64[352];

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
"#cv{width:100%;height:70vh;background:var(--panel);border:1px solid var(--border);border-radius:8px}\n"
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
"<div class='card'><h3 data-i18n='range'>Plage TWS</h3>\n"
"<label><span data-i18n='from'>De</span> <select id='from'></select></label>\n"
"<label><span data-i18n='to'>à</span> <select id='to'></select></label></div>\n"
"<div class='card'><h3 data-i18n='legend'>Légende (TWS)</h3><div id='leg' class='lg'></div></div>\n"
"<div class='card'><small id='info'></small></div>\n"
"</aside></main>\n"
"<script>\n"
"let lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"let theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"const L={fr:{range:'Plage TWS',from:'De',to:'à',legend:'Légende (TWS)',kn:'nœuds',empty:'Aucune polaire chargée.',max:'Vitesse max'},\n"
"en:{range:'TWS range',from:'From',to:'to',legend:'Legend (TWS)',kn:'knots',empty:'No polar loaded.',max:'Max speed'}};\n"
"const T=k=>(L[lang]&&L[lang][k]!=null)?L[lang][k]:k;\n"
"function i18n(){document.querySelectorAll('[data-i18n]').forEach(e=>e.textContent=T(e.dataset.i18n));document.documentElement.lang=lang;}\n"
"const $=s=>document.querySelector(s);\n"
"let P=null;\n"
"function col(i,n){return 'hsl('+Math.round(360*i/Math.max(1,n))+',70%,55%)';}\n"
"function spline(x,p){const t=0.5;x.beginPath();x.moveTo(p[0][0],p[0][1]);\n"  /* t=0.5 : même tension que diagram.c (GTK) */
" for(let i=0;i<p.length-1;i++){const p1=p[i],p2=p[i+1];\n"
"  const p0=i?p[i-1]:[2*p1[0]-p2[0],2*p1[1]-p2[1]];\n"               /* réflexion aux bords, comme GTK */
"  const p3=(i+2<p.length)?p[i+2]:[2*p2[0]-p1[0],2*p2[1]-p1[1]];\n"
"  const c1x=p1[0]+(p2[0]-p0[0])*t/6,c1y=p1[1]+(p2[1]-p0[1])*t/6,c2x=p2[0]-(p3[0]-p1[0])*t/6,c2y=p2[1]-(p3[1]-p1[1])*t/6;\n"
"  x.bezierCurveTo(c1x,c1y,c2x,c2y,p2[0],p2[1]);}x.stroke();}\n"
"function opt(sel,arr,val){sel.innerHTML=arr.map((v,i)=>'<option value='+i+(i===val?' selected':'')+'>'+v+' '+T('kn')+'</option>').join('');}\n"
"function shownIdx(){let a=+$('#from').value,b=+$('#to').value;if(a>b){const t=a;a=b;b=t;}const r=[];for(let i=a;i<=b;i++)r.push(i);return r;}\n"
"function draw(){const c=$('#cv'),x=c.getContext('2d');const W=c.width=c.clientWidth,H=c.height=c.clientHeight;\n"
" const cs=getComputedStyle(document.body),fg=cs.getPropertyValue('--fg'),bd=cs.getPropertyValue('--border'),mu=cs.getPropertyValue('--muted');\n"
" x.clearRect(0,0,W,H);if(!P||!P.tws.length){x.fillStyle=mu;x.fillText(T('empty'),20,30);return;}\n"
" const idx=shownIdx();let mx=0;for(const s of idx)for(let a=0;a<P.twa.length;a++)mx=Math.max(mx,P.bsp[a][s]);\n"
" if(mx<=0)mx=1;const ring=Math.max(1,Math.ceil(mx/5));const top=Math.ceil(mx/ring)*ring;\n"
" const cx=W*0.16,cy=H*0.5,R=Math.min(H*0.44,W*0.78);\n"
" const px=(twa,bsp)=>[cx+R*bsp/top*Math.sin(twa*Math.PI/180),cy-R*bsp/top*Math.cos(twa*Math.PI/180)];\n"
" x.strokeStyle=bd;x.fillStyle=mu;x.font='12px system-ui';x.textAlign='left';\n"
" for(let r=ring;r<=top+0.001;r+=ring){x.beginPath();for(let t=0;t<=180;t+=2){const p=px(t,r);t===0?x.moveTo(p[0],p[1]):x.lineTo(p[0],p[1]);}x.stroke();\n"
"  const lp=px(0,r);x.fillText(r,lp[0]+3,lp[1]+3);}\n"
" for(let t=0;t<=180;t+=30){x.beginPath();x.moveTo(cx,cy);const p=px(t,top);x.lineTo(p[0],p[1]);x.stroke();\n"
"  const lp=px(t,top*1.06);x.fillText(t+'°',lp[0]-6,lp[1]);}\n"
" idx.forEach((s,k)=>{const pts=[];for(let a=0;a<P.twa.length;a++){const b=P.bsp[a][s];if(b>0)pts.push(px(P.twa[a],b));}\n"
"  if(pts.length<2)return;x.strokeStyle=col(k,idx.length);x.lineWidth=2;spline(x,pts);});\n"
" x.lineWidth=1;\n"
" $('#leg').innerHTML=idx.map((s,k)=>'<div><span class=sw style=\"background:'+col(k,idx.length)+'\"></span>'+P.tws[s]+' '+T('kn')+'</div>').join('');\n"
" $('#info').textContent=T('max')+' : '+mx.toFixed(2)+' '+T('kn');\n"
"}\n"
"async function load(){try{const r=await fetch('/api/polar');P=await r.json();}catch(e){P=null;}\n"
" $('#fn').textContent=P&&P.filename?(' — '+P.filename):'';\n"
" if(P&&P.tws.length){opt($('#from'),P.tws,0);opt($('#to'),P.tws,P.tws.length-1);}draw();}\n"
"$('#from').onchange=draw;$('#to').onchange=draw;addEventListener('resize',draw);\n"
"function applyTheme(){document.body.classList.toggle('light',theme==='light');$('#theme').textContent=theme==='dark'?'☀':'🌙';}\n"
"$('#lang').onclick=()=>{lang=lang==='fr'?'en':'fr';localStorage.setItem('lang',lang);$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();load();};\n"
"$('#theme').onclick=()=>{theme=theme==='dark'?'light':'dark';localStorage.setItem('theme',theme);applyTheme();draw();};\n"
"applyTheme();$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();load();\n"
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
    if (!g_loaded) { send_text(fd, 200, "OK", "application/json", "{\"filename\":\"\",\"tws\":[],\"twa\":[],\"bsp\":[]}"); return; }

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
    APP("]}");
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
                "Usage : %s [fichier.pol] [--port N] [--bind ADDR] [--auth user:pass]\n"
                "  --port N     port d'écoute (défaut 8081 ; 8080 = n2k-mux-web)\n"
                "  --bind ADDR  adresse d'écoute (défaut 127.0.0.1 ; 0.0.0.0 = LAN)\n"
                "  --auth u:p   authentification HTTP Basic\n", argv[0]);
            return 0;
        } else if (argv[i][0] != '-') pol = argv[i];
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); return 2; }
    }

    init_polar_data(&g_polar);
    if (pol) {
        if (load_polar_file(pol, &g_polar)) g_loaded = 1;
        else fprintf(stderr, "polar_doctor_web : impossible de charger %s\n", pol);
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
