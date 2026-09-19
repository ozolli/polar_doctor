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

/* Windows : winsock2.h doit précéder windows.h, tiré par libpolar.h. */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601          /* Windows 7+ : WSAPoll, inet_pton */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif
#include "libpolar.h"

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <bcrypt.h>
#  define poll WSAPoll                   /* même struct pollfd / POLLIN / POLLHUP */
#  define sock_close closesocket
#else
#  include <poll.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <termios.h>
#  define sock_close close
#endif
#ifndef O_BINARY
#  define O_BINARY 0                     /* Windows : écrire les octets tels quels */
#endif

/* ------------------------------------------------ portabilité POSIX / Windows ---
 * Les sockets sont manipulées en int : sous Windows, SOCKET est un handle noyau
 * de petite valeur (INVALID_SOCKET converti en int vaut -1, donc « fd < 0 » reste
 * valable). Les fichiers passent par GLib (g_open, g_fopen, g_unlink, g_mkdir,
 * GDir, g_file_test) : chemins UTF-8, y compris accentués, sous Windows. */
#ifdef _WIN32
static char *strcasestr(const char *h, const char *n)       /* absent de MinGW */
{
    size_t l = strlen(n);
    for (; *h; h++) if (g_ascii_strncasecmp(h, n, l) == 0) return (char *)h;
    return l ? NULL : (char *)h;
}
#endif

/* Écriture sur une socket (write() n'agit pas sur une socket Winsock). */
static void sock_write(int fd, const void *buf, size_t len)
{
    int w = (int)send(fd, (const char *)buf, (int)len, 0); (void)w;
}

static void sock_timeouts(int fd, int sec)
{
#ifdef _WIN32
    DWORD ms = (DWORD)sec * 1000;                           /* Winsock : DWORD en ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

/* ------------------------------------------------------------ port série ---
 * Passerelle Actisense NGX-1/NGT-1 : 8N1, sans contrôle de flux, lecture non
 * bloquante (la boucle poll() relève le port toutes les 50 ms). Spécification
 * « périphérique[@débit] » : /dev/ttyUSB0, /dev/serial/by-id/…, COM3, COM12@230400.
 * Débit par défaut 115200 (NGX-1 en sortie d'usine). */
#ifdef _WIN32
#  define SERIAL_DEFAULT "COM3"
static HANDLE g_ser = INVALID_HANDLE_VALUE;
#else
#  define SERIAL_DEFAULT "/dev/ttyUSB0"
static int g_ser = -1;
#endif

static void serial_close(void)
{
#ifdef _WIN32
    if (g_ser != INVALID_HANDLE_VALUE) CloseHandle(g_ser);
    g_ser = INVALID_HANDLE_VALUE;
#else
    if (g_ser >= 0) close(g_ser);
    g_ser = -1;
#endif
}

/* Ouvre le port ; false + message dans err en cas d'échec. */
static bool serial_open(const char *spec, char *err, size_t errsz)
{
    char dev[160]; long baud = 115200;
    snprintf(dev, sizeof dev, "%s", spec);
    char *at = strrchr(dev, '@');
    if (at) { *at = 0; baud = strtol(at + 1, NULL, 10); }
    if (!dev[0]) { snprintf(err, errsz, "port série non précisé"); return false; }
    serial_close();
#ifdef _WIN32
    char path[180];
    if (strncmp(dev, "\\\\.\\", 4) == 0) snprintf(path, sizeof path, "%s", dev);
    else snprintf(path, sizeof path, "\\\\.\\%s", dev);        /* \\.\COM12 : obligatoire au-delà de COM9 */
    wchar_t *w = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    g_ser = w ? CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    g_free(w);
    if (g_ser == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, errsz, "%.100s : %s", dev, e == ERROR_FILE_NOT_FOUND ? "port inexistant"
                 : e == ERROR_ACCESS_DENIED ? "port déjà utilisé par un autre programme" : "ouverture impossible");
        return false;
    }
    SetupComm(g_ser, 65536, 4096);                   /* tampon d'entrée large : relevé toutes les 50 ms */
    DCB dcb = { .DCBlength = sizeof dcb };
    GetCommState(g_ser, &dcb);
    dcb.BaudRate = (DWORD)baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    COMMTIMEOUTS to = { .ReadIntervalTimeout = MAXDWORD, .WriteTotalTimeoutConstant = 500 };  /* lecture immédiate */
    if (!SetCommState(g_ser, &dcb) || !SetCommTimeouts(g_ser, &to)) {
        snprintf(err, errsz, "%.100s : réglage à %ld bauds refusé", dev, baud);
        serial_close(); return false;
    }
    PurgeComm(g_ser, PURGE_RXCLEAR);
#else
    speed_t sp;
    switch (baud) {
    case 38400: sp = B38400; break;
    case 57600: sp = B57600; break;
    case 115200: sp = B115200; break;
    case 230400: sp = B230400; break;
#  ifdef B460800
    case 460800: sp = B460800; break;
#  endif
    default: snprintf(err, errsz, "débit non géré : %ld (38400, 57600, 115200, 230400, 460800)", baud); return false;
    }
    g_ser = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_ser < 0) {
        snprintf(err, errsz, "%.100s : %s%s", dev, strerror(errno),
                 errno == EACCES ? " (ajouter l'utilisateur du serveur au groupe dialout)" : "");
        return false;
    }
    struct termios t;
    if (tcgetattr(g_ser, &t) != 0) { snprintf(err, errsz, "%.100s : pas un port série", dev); serial_close(); return false; }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);
    cfsetispeed(&t, sp); cfsetospeed(&t, sp);
    tcflush(g_ser, TCIFLUSH);
    if (tcsetattr(g_ser, TCSANOW, &t) != 0) { snprintf(err, errsz, "%.100s : réglage refusé", dev); serial_close(); return false; }
#endif
    return true;
}

/* Lecture non bloquante : >0 octets lus, 0 rien de disponible, -1 port perdu. */
static long serial_read(uint8_t *buf, size_t cap)
{
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(g_ser, buf, (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
#else
    ssize_t r = read(g_ser, buf, cap);
    if (r > 0) return (long)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;                                      /* 0 = fin de fichier : adaptateur débranché */
#endif
}

static void serial_write(const uint8_t *buf, size_t len)
{
#ifdef _WIN32
    DWORD w = 0; WriteFile(g_ser, buf, (DWORD)len, &w, NULL);
#else
    ssize_t w = write(g_ser, buf, len); (void)w;
#endif
}

/* Remplacement atomique d'un fichier : sous Windows, rename() ÉCHOUE si la cible
 * existe, ce qui casserait l'enregistrement « .tmp puis remplace ». */
static int replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    gunichar2 *wf = g_utf8_to_utf16(from, -1, NULL, NULL, NULL);
    gunichar2 *wt = g_utf8_to_utf16(to, -1, NULL, NULL, NULL);
    int ok = wf && wt && MoveFileExW((LPCWSTR)wf, (LPCWSTR)wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    g_free(wf); g_free(wt);
    return ok ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static void local_tm(time_t t, struct tm *out)
{
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

/* Octets aléatoires cryptographiques (secret de session). */
static bool rand_bytes(unsigned char *b, size_t n)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, b, n);
    close(fd);
    return r == (ssize_t)n;
#endif
}

/* Dernier séparateur de chemin : '/' partout, '\\' aussi sous Windows. */
static char *last_sep(const char *p)
{
    char *sep = strrchr(p, '/');
#ifdef _WIN32
    char *bs = strrchr(p, '\\');
    if (!sep || (bs && bs > sep)) sep = bs;
#endif
    return sep;
}

#define REQ_MAX   131072   /* en-têtes + corps POST (.pol édité) */
#define JSON_MAX  262144   /* polaire en JSON (worst case ~110 Ko) */

static PolarData    g_polar;
static int          g_loaded = 0;
static const char  *g_auth = NULL;     /* "user:pass" attendu (NULL = pas d'auth) */
static char         g_auth_b64[352];
#define AUTH_RAW_MAX 256                   /* longueur max acceptée de "user:pass" */
static char         g_token[65] = "";     /* cookie de session = HMAC-SHA256(secret, user:pass) */

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
".sl{display:inline-block;font-size:.82em;white-space:nowrap;margin:0 .6em .15em 0}\n"
".orph{color:#d4880f;font-style:italic}\n"
"#cfgform input[type=text],#cfgform input:not([type]){background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.15em .3em}\n"
"@media print{header nav,#prt,#help,#logout,#lang,#theme,aside,#hmodal{display:none!important}#wrap{flex:1 1 100%}}\n"
"#hbody{font-size:.93em;line-height:1.5}#hbody h2{margin:.2em 0 .6em}#hbody h3{color:var(--accent);margin:1em 0 .3em;font-size:1em}\n"
"#hbody li{margin:.15em 0}#hbody ul,#hbody ol{margin:.2em 0 .2em 1.2em;padding:0}\n"
"</style></head><body>\n"
"<header><b>Polar Doctor</b><small id='fn'></small>\n"
"<nav><button data-v='diag' class='on' data-i18n='tabdiag'>Diagramme</button><button data-v='data' data-i18n='tabdata'>Données</button><button data-v='vmg' data-i18n='tabvmg'>VMG</button><button data-v='cfg' data-i18n='tabcfg'>Bateau</button></nav>\n"
"<span style='margin-left:auto;display:flex;gap:.5em'>"
"<button id='logout' class='hbtn' title='Déconnexion' style='display:none'>⏻</button>"
"<button id='help' class='hbtn' title='Aide'>?</button>"
"<button id='prt' class='hbtn' title='Imprimer / PDF'>⎙</button>"
"<button id='lang' class='hbtn'></button><button id='theme' class='hbtn'></button></span></header>\n"
"<main id='mdiag'>\n"
"<div id='wrap'><canvas id='cv'></canvas></div>\n"
"<aside>\n"
"<div class='card'><h3 data-i18n='boat'>Bateau</h3>\n"
"<div id='bname' style='font-size:.9em;margin-bottom:.3em'></div>\n"
"<select id='polsel'></select>\n"
"<label style='margin-top:.4em'><span data-i18n='recent'>Récents</span> <select id='recsel'></select></label>\n"
"<input id='bfolder' placeholder='/chemin/dossier' style='width:100%;margin-top:.3em;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.15em .3em'>\n"
"<div style='margin-top:.3em;display:flex;gap:.3em;align-items:center'>"
"<button class='hbtn' id='btnOpen' data-i18n='open1'>Ouvrir</button>"
"<input id='bnewname' placeholder='Nom' style='width:6em;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:.15em .3em'>"
"<button class='hbtn' id='btnNew' data-i18n='new1'>Nouveau</button></div>\n"
"<div id='bmsg' style='font-size:.8em;color:var(--muted);margin-top:.2em'></div></div>\n"
"<div class='card'><h3 data-i18n='range'>Plage TWS</h3>\n"
"<label><span data-i18n='from'>De</span> <select id='from'></select></label>\n"
"<label><span data-i18n='to'>à</span> <select id='to'></select></label></div>\n"
"<div class='card'><h3 data-i18n='dyn'>Mode dynamique</h3>\n"
"<label><input type=checkbox id='dyn'> <span data-i18n='dyn_on'>Activer</span></label>\n"
"<label><span data-i18n='tws1'>TWS</span> <input type=number id='dtws' value='10' min='0' step='0.5' style='width:5em'> <span data-i18n='kn'>nœuds</span></label>\n"
"<div id='read' style='font-size:.85em;margin-top:.4em'></div></div>\n"
"<div class='card'><h3 data-i18n='live'>Live</h3>\n"
"<label><span data-i18n='source'>Source</span> <select id='lvsrc'><option value='udp'>UDP (0183 / N2K)</option><option value='tcp'>TCP (0183 / N2K)</option><option value='vdr'>VDR qtVlm</option><option value='ngx'>Actisense NGX-1 (série)</option></select></label>\n"
"<label><input id='lvaddr' value='10110' style='width:9em' title='UDP: port · TCP: hôte:port · VDR: chemin .db · NGX-1: port série[@débit]'></label>\n"
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
"<button class='hbtn' id='btnCreate' data-i18n='create1'>Créer</button> <button class='hbtn' id='btnUpdate' data-i18n='update1'>Mettre à jour</button> <label class=sl data-i18n-title='pcthint' style='margin-left:.6em'>Percentile <select id='pctsel'></select></label> <span id='impmsg' style='font-size:.85em;color:var(--muted)'></span></div>\n"
"<div id='dtable' style='overflow:auto'></div></div>\n"
"<div id='mvmg' style='display:none;padding:1em'><div id='vmgtable' style='overflow:auto'></div></div>\n"
"<div id='mcfg' style='display:none;padding:1em;max-width:900px'>\n"
"<div style='margin-bottom:.6em'><button class='hbtn' id='btnCfgSave' data-i18n='save1'>Enregistrer</button> "
"<button class='hbtn' id='btnAddPolar' data-i18n='addpolar'>+ Polaire</button> "
"<span id='cfgmsg' style='font-size:.85em;color:var(--muted)'></span></div>\n"
"<div id='cfgform'></div></div>\n"
"<div id='hmodal' style='display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:20'>\n"
"<div style='max-width:820px;margin:3vh auto;background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:1em 1.4em;max-height:94vh;overflow:auto'>\n"
"<button class='hbtn' id='hclose' style='float:right'>✕</button><div id='hbody'></div></div></div>\n"
"<script>\n"
"const _fetch=window.fetch;window.fetch=async(...a)=>{const r=await _fetch(...a);if(r.status===401)location.href='/';return r;};\n"  /* session expirée -> connexion */
"let lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"let theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"const L={fr:{boat:'Bateau',range:'Plage TWS',from:'De',to:'à',legend:'Légende (TWS)',kn:'nœuds',empty:'Aucune polaire chargée.',max:'Vitesse max',dyn:'Mode dynamique',dyn_on:'Activer',tws1:'TWS',live:'Live',source:'Source',start:'Démarrer',stop:'Arrêter',moteur:'Moteur',main1:'GV',head1:'Voile av.',sea1:'Mer',tabdiag:'Diagramme',tabdata:'Données',save1:'Enregistrer',addtwa:'+ TWA',addtws:'+ TWS',import:'Import fichiers',create1:'Créer',update1:'Mettre à jour',tabvmg:'VMG',vmgup:'Près',vmgdn:'Portant',recent:'Récents',open1:'Ouvrir',new1:'Nouveau',tabcfg:'Bateau',addpolar:'+ Polaire',cfgname:'Nom',charge1:'Charge',polars1:'Polaires',orphan:'Absent de l’inventaire — décochez pour le retirer',orphhint:'⚠ En orange : critères absents de l’inventaire. Décochez-les pour les retirer, ou ajoutez-les à l’inventaire.'},\n"
"en:{boat:'Boat',range:'TWS range',from:'From',to:'to',legend:'Legend (TWS)',kn:'knots',empty:'No polar loaded.',max:'Max speed',dyn:'Dynamic mode',dyn_on:'Enable',tws1:'TWS',live:'Live',source:'Source',start:'Start',stop:'Stop',moteur:'Engine',main1:'Main',head1:'Headsail',sea1:'Sea',tabdiag:'Diagram',tabdata:'Data',save1:'Save',addtwa:'+ TWA',addtws:'+ TWS',import:'Import files',create1:'Create',update1:'Update',tabvmg:'VMG',vmgup:'Upwind',vmgdn:'Downwind',recent:'Recent',open1:'Open',new1:'New',tabcfg:'Boat',addpolar:'+ Polar',cfgname:'Name',charge1:'Charge',polars1:'Polars',orphan:'Not in the inventory — uncheck to remove it',orphhint:'⚠ In orange: criteria not in the inventory. Uncheck them to remove them, or add them to the inventory.'}};\n"
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
"async function loadBoats(){try{const b=await fetch('/api/boats').then(r=>r.json());const rec=b.recent||[];\n"
" $('#recsel').innerHTML='<option value=\"\">—</option>'+rec.map(p=>'<option value=\"'+p+'\"'+(p===b.current?' selected':'')+'>'+(p.split('/').filter(Boolean).pop()||p)+'</option>').join('');\n"
" if(b.current)$('#bfolder').value=b.current;$('#logout').style.display=b.auth?'':'none';}catch(e){}}\n"
"async function openBoat(f){if(!f)return;$('#bmsg').textContent='…';\n"
" const r=await fetch('/api/open?folder='+encodeURIComponent(f));\n"
" if(r.ok){$('#bmsg').textContent='';await loadBoats();await loadBoat();await load();renderTable();}else $('#bmsg').textContent='✗';}\n"
"$('#recsel').onchange=e=>{if(e.target.value)openBoat(e.target.value);};\n"
"$('#btnOpen').onclick=()=>openBoat($('#bfolder').value.trim());\n"
"$('#btnNew').onclick=async()=>{const f=$('#bfolder').value.trim(),nm=$('#bnewname').value.trim();if(!f)return;$('#bmsg').textContent='…';\n"
" const r=await fetch('/api/newboat?folder='+encodeURIComponent(f)+'&name='+encodeURIComponent(nm));const d=await r.json().catch(()=>({}));\n"
" if(d.ok){$('#bmsg').textContent='✓';await loadBoats();await loadBoat();await load();renderTable();}else $('#bmsg').textContent='✗';};\n"
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
" else $('#lvinfo').textContent=(LIVE&&LIVE.err)?('⚠ '+LIVE.err):(LIVE&&LIVE.saved)?('✓ '+LIVE.saved.split('/').pop()):'';\n"
" const on=!!(LIVE&&LIVE.on);$('#lvbtn').textContent=on?T('stop'):T('start');$('#lvbtn').dataset.on=on?'1':'';\n"
" const mot=!!(LIVE&&LIVE.moteur);$('#lvmot').dataset.on=mot?'1':'';$('#lvmot').style.background=mot?'var(--active)':'';$('#lvmot').style.color=mot?'#fff':'';\n"
" draw();}\n"
"$('#lvbtn').onclick=async()=>{if($('#lvbtn').dataset.on==='1'){await fetch('/api/live/stop');stopPoll();await loadBoat();await pollLive();}\n"
" else{await fetch('/api/live/start?src='+$('#lvsrc').value+'&addr='+encodeURIComponent($('#lvaddr').value));startPoll();await pollLive();}};\n"
"$('#lvmot').onclick=async()=>{const on=$('#lvmot').dataset.on==='1'?0:1;await fetch('/api/live/moteur?on='+on);await pollLive();};\n"
"$('#lvsrc').onchange=async()=>{const v=$('#lvsrc').value,a=$('#lvaddr');a.placeholder=v==='vdr'?'…/vdrs/vdr.db':'';\n"
" if(v==='vdr'){let d='';try{d=(await fetch('/api/live').then(r=>r.json())).vdr_default||'';}catch(e){}a.value=d;}\n"
" else a.value=v==='ngx'?'" SERIAL_DEFAULT "':'10110';};\n"
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
"$('#pctsel').innerHTML=[85,86,87,88,89,90,91,92,93,94,95].map(p=>'<option value='+p+'>P'+p+'</option>').join('');\n"
"fetch('/api/percentile').then(r=>r.json()).then(d=>{$('#pctsel').value=d.p;}).catch(()=>{});\n"
"$('#pctsel').onchange=e=>fetch('/api/percentile?p='+e.target.value);\n"
"let CFG=null;\n"
"const esc=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\"/g,'&quot;');\n"
"async function loadCfg(){try{CFG=await fetch('/api/config').then(r=>r.json());}catch(e){CFG=null;}renderCfg();}\n"
"const ci=(a,v)=>(a||[]).findIndex(x=>String(x).toLowerCase()===String(v).toLowerCase());\n"  /* = g_ascii_strcasecmp de boat_in_list */
"const orphOf=(all,sel)=>(sel||[]).filter(v=>ci(all,v)<0);\n"
"function cfgGrp(dim,i,all,sel){const lbl=dim==='mains'?T('main1'):dim==='heads'?T('head1'):T('sea1');\n"
" const box=(v,orph)=>'<label class=\"sl'+(orph?' orph':'')+'\"'+(orph?' title=\"'+esc(T('orphan'))+'\"':'')+'><input type=checkbox class=pcb data-i='+i+' data-d='+dim+' value=\"'+esc(v)+'\"'+(orph||ci(sel,v)>=0?' checked':'')+'> '+esc(v)+(orph?' ⚠':'')+'</label>';\n"
" return '<div style=\"margin-top:.3em\"><small>'+lbl+'</small><br>'+(all||[]).map(v=>box(v,false)).join('')+orphOf(all,sel).map(v=>box(v,true)).join('')+'</div>';}\n"
"function renderCfg(){if(!CFG){$('#cfgform').innerHTML='';return;}\n"
" const ta=(id,label,arr)=>'<div class=card><h3>'+label+'</h3><textarea id='+id+' rows=5 style=\"width:100%;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:4px\">'+esc((arr||[]).join('\\n'))+'</textarea></div>';\n"
" let h='<div class=card><label>'+T('cfgname')+' <input id=cfgname value=\"'+esc(CFG.name||'')+'\"></label></div>';\n"
" h+=ta('cfgmains',T('main1'),CFG.mains)+ta('cfgheads',T('head1'),CFG.heads)+ta('cfgseas',T('sea1'),CFG.seas);\n"
" h+='<div class=card><h3>'+T('moteur')+'</h3><label>'+T('moteur')+' <input id=cfgmot value=\"'+esc(CFG.moteur||'')+'\"></label> <label>'+T('charge1')+' <input id=cfgchg value=\"'+esc(CFG.charge||'')+'\"></label></div>';\n"
" h+='<div class=card><h3>'+T('polars1')+'</h3>';\n"
" if((CFG.polars||[]).some(p=>orphOf(CFG.mains,p.mains).length||orphOf(CFG.heads,p.heads).length||orphOf(CFG.seas,p.seas).length))h+='<p class=orph style=\"font-size:.85em\">'+T('orphhint')+'</p>';\n"
" (CFG.polars||[]).forEach((p,i)=>{h+='<div class=card><label>'+T('cfgname')+' <input class=pname data-i='+i+' value=\"'+esc(p.name||'')+'\"></label> <button class=delx data-p='+i+'>✕</button>'+cfgGrp('mains',i,CFG.mains,p.mains)+cfgGrp('heads',i,CFG.heads,p.heads)+cfgGrp('seas',i,CFG.seas,p.seas)+'</div>';});\n"
" $('#cfgform').innerHTML=h+'</div>';}\n"
"$('#cfgform').addEventListener('change',e=>{const t=e.target;if(!CFG)return;const lines=v=>v.split('\\n').map(s=>s.trim()).filter(Boolean);\n"
" if(t.id==='cfgname')CFG.name=t.value.trim();\n"
" else if(t.id==='cfgmot')CFG.moteur=t.value.trim();\n"
" else if(t.id==='cfgchg')CFG.charge=t.value.trim();\n"
" else if(t.id==='cfgmains'){CFG.mains=lines(t.value);renderCfg();}\n"
" else if(t.id==='cfgheads'){CFG.heads=lines(t.value);renderCfg();}\n"
" else if(t.id==='cfgseas'){CFG.seas=lines(t.value);renderCfg();}\n"
" else if(t.classList.contains('pname'))CFG.polars[+t.dataset.i].name=t.value.trim();\n"
" else if(t.classList.contains('pcb')){const p=CFG.polars[+t.dataset.i],d=t.dataset.d,a=p[d]||(p[d]=[]);const j=ci(a,t.value);\n"
"  if(t.checked){if(j<0)a.push(t.value);}else if(j>=0)a.splice(j,1);}});\n"
"$('#cfgform').addEventListener('click',e=>{if(!e.target.classList.contains('delx'))return;const i=+e.target.dataset.p;if(isNaN(i))return;CFG.polars.splice(i,1);renderCfg();});\n"
"$('#btnAddPolar').onclick=()=>{if(!CFG)return;(CFG.polars=CFG.polars||[]).push({name:'Polaire'+((CFG.polars||[]).length+1),mains:[],heads:[],seas:[]});renderCfg();};\n"
"function genCfg(){let s='[boat]\\nname = '+CFG.name+'\\n\\n[mainsail]\\n'+(CFG.mains||[]).join('\\n')+'\\n\\n[headsails]\\n'+(CFG.heads||[]).join('\\n')+'\\n\\n[seastates]\\n'+(CFG.seas||[]).join('\\n')+'\\n\\n[engine]\\nmoteur = '+(CFG.moteur||'')+'\\ncharge = '+(CFG.charge||'')+'\\n\\n[polars]\\n';\n"
" (CFG.polars||[]).forEach(p=>{s+=p.name+' = mains: '+(p.mains&&p.mains.length?p.mains.join(', '):'*')+' ; heads: '+(p.heads&&p.heads.length?p.heads.join(', '):'*')+' ; seas: '+(p.seas&&p.seas.length?p.seas.join(', '):'*')+'\\n';});return s;}\n"
"$('#btnCfgSave').onclick=async()=>{if(!CFG)return;$('#cfgmsg').textContent='…';\n"
" const r=await fetch('/api/config',{method:'POST',body:genCfg()});const d=await r.json().catch(()=>({}));\n"
" if(d.ok){$('#cfgmsg').textContent='✓';await loadCfg();await loadBoat();}else $('#cfgmsg').textContent='✗';};\n"
"document.querySelectorAll('header nav button').forEach(b=>b.onclick=()=>{const v=b.dataset.v;$('#mdiag').style.display=v==='diag'?'':'none';$('#mdata').style.display=v==='data'?'':'none';$('#mvmg').style.display=v==='vmg'?'':'none';$('#mcfg').style.display=v==='cfg'?'':'none';document.querySelectorAll('header nav button').forEach(x=>x.classList.toggle('on',x===b));if(v==='data')renderTable();if(v==='vmg')renderVmg();if(v==='cfg')loadCfg();});\n"
"$('#prt').onclick=()=>window.print();\n"
"const HELP={fr:`\n"
"<h2>Polar Doctor — Guide</h2>\n"
"<h3>À quoi sert ce programme ?</h3>\n"
"<p>Il construit la <b>polaire</b> de votre voilier : un tableau et un diagramme donnant la vitesse du bateau pour chaque combinaison d'angle et de force du vent. C'est la carte d'identité de performance du bateau, utilisée par les logiciels de routage et pour comparer vos navigations.</p>\n"
"<p>Elle est fabriquée à partir de vos données <b>réelles</b> : fichiers NMEA0183, bases VDR de qtVlm, ou <b>capture en direct</b> pendant que vous naviguez. Plus vous accumulez de données dans des conditions variées, plus elle est fidèle.</p>\n"
"<h3>Le vocabulaire en 30 secondes</h3>\n"
"<ul>\n"
"<li><b>TWA</b> — angle du vent réel par rapport à l'axe du bateau, de 0° (vent debout) à 180° (vent arrière)</li>\n"
"<li><b>TWS</b> — force du vent réel, en nœuds</li>\n"
"<li><b>STW / BSP</b> — vitesse du bateau dans l'eau (loch) : c'est ce que la polaire représente</li>\n"
"<li><b>SOG</b> — vitesse par rapport au fond (GPS) ; sert à fiabiliser le STW</li>\n"
"<li><b>VMG</b> — composante de la vitesse dans l'axe du vent : l'efficacité au près et au portant</li>\n"
"<li><b>AWA / AWS</b> — le vent apparent, ressenti à bord</li>\n"
"</ul>\n"
"<h3>Prise en main</h3>\n"
"<ol>\n"
"<li>Carte <b>Bateau</b> : choisissez un bateau <b>récent</b>, ou saisissez un dossier et <b>Ouvrir</b>, ou créez-en un avec <b>Nouveau</b>. Un bateau = un dossier contenant sa config et ses polaires.</li>\n"
"<li>Onglet <b>Bateau</b> : renseignez l'inventaire (grand-voile, voiles d'avant, états de mer) et définissez vos <b>polaires</b> avec leurs critères.</li>\n"
"<li>Alimentez-la : onglet <b>Données</b> → <b>Import fichiers</b> (Créer / Mettre à jour), ou carte <b>Live</b> en navigation.</li>\n"
"<li>Lisez le résultat (<b>Diagramme</b>, <b>VMG</b>), corrigez dans <b>Données</b>, imprimez en PDF avec ⎙.</li>\n"
"</ol>\n"
"<h3>Les onglets</h3>\n"
"<ul>\n"
"<li><b>Diagramme</b> — une courbe par TWS (plage réglable). La couleur marque la plage <b>VMG utile</b>, le <b>rouge</b> le VMG dégradé (près trop serré, portant trop bas). <b>Mode dynamique</b> : saisissez une TWS quelconque et survolez le diagramme pour lire TWA / AWA / AWS / BS / VMG.</li>\n"
"<li><b>Données</b> — tableau éditable : une ligne par TWA, une colonne par TWS. Modifiez les cellules, ajoutez ou supprimez lignes et colonnes, puis <b>Enregistrer</b>.</li>\n"
"<li><b>VMG</b> — meilleurs angles au près et au portant pour chaque TWS.</li>\n"
"<li><b>Bateau</b> — inventaire et polaires. Pour chaque polaire, ses critères en cases à cocher ; <b>ne rien cocher = tout</b>.</li>\n"
"</ul>\n"
"<h3>Capture live</h3>\n"
"<p>Carte <b>Live</b> : choisissez la source — <b>UDP</b> (port d'écoute), <b>TCP</b> (hôte:port) ou <b>VDR qtVlm</b> (chemin du .db) ou <b>Actisense NGX-1</b> (port série : <code>/dev/ttyUSB0</code>, <code>COM3</code>, <code>@230400</code> pour changer de débit ; passerelle en mode Transfer) — puis <b>Démarrer</b>. En UDP/TCP, le format est reconnu tout seul : <b>NMEA 0183</b> ou <b>NMEA 2000</b> au format texte YDRAW (n2k-mux port 2700, passerelles Yacht Devices).</p>\n"
"<p>Réglez en direct l'état du bateau (grand-voile, voile d'avant, état de mer) : chaque point est routé vers <b>toutes</b> les polaires dont les critères correspondent. Le bouton <b>Moteur</b> suspend l'enregistrement quand l'hélice est embrayée.</p>\n"
"<p>Le nuage gris montre les points bruts, le point rouge la mesure courante, et la courbe se construit sous vos yeux. À l'<b>arrêt</b>, chaque polaire alimentée est enregistrée.</p>\n"
"<h3>Comment c'est calculé</h3>\n"
"<ul>\n"
"<li>les points sont groupés par cases de 5° (TWA) et 2 nœuds (TWS), minimum 3 points par case</li>\n"
"<li>on retient un <b>percentile élevé</b> (P90 par défaut, réglable de P85 à P95 dans l’onglet <b>Données</b>) : la performance <i>atteignable</i>, pas la moyenne</li>\n"
"<li>le STW est lissé puis débruité par comparaison au SOG (les sauts du loch sont rejetés)</li>\n"
"<li>les points sous moteur sont exclus</li>\n"
"</ul>\n"
"<h3>Formats de fichiers</h3>\n"
"<ul>\n"
"<li><b>NMEA</b> (.nmea, .log, .txt) — vent MWV ou MWD, vitesse surface VHW, cap HDT/HDG ; SOG lu s'il est présent</li>\n<li><b>NMEA 2000</b> (journal texte YDRAW) — vent 130306, vitesse surface 128259, SOG 129026, cap 127250</li>\n"
"<li><b>VDR</b> (.db) — base SQLite de qtVlm</li>\n"
"<li><b>Polaire</b> (.pol) — tableau à point-virgule</li>\n"
"</ul>\n"
"<h3>Conseils</h3>\n"
"<ul>\n"
"<li>Combinez plusieurs sorties, dans des conditions variées.</li>\n"
"<li>Une case vide signifie simplement : pas encore assez de données.</li>\n"
"<li><b>Mettre à jour</b> ré-agrège l'existant avec les nouvelles données : la polaire peut monter <i>ou</i> baisser — pratique pour ramener une polaire théorique VPP au réel.</li>\n"
"</ul>\n"
"`,en:`\n"
"<h2>Polar Doctor — Guide</h2>\n"
"<h3>What is this program for?</h3>\n"
"<p>It builds your sailboat <b>polar</b>: a table and a diagram giving the boat speed for every combination of wind angle and wind strength. It is the boat performance fingerprint, used by routing software and to compare your sailing.</p>\n"
"<p>It is built from your <b>real</b> data: NMEA0183 files, qtVlm VDR databases, or <b>live capture</b> while sailing. The more data you gather in varied conditions, the more accurate it gets.</p>\n"
"<h3>Vocabulary in 30 seconds</h3>\n"
"<ul>\n"
"<li><b>TWA</b> — true wind angle relative to the boat axis, 0° (head to wind) to 180° (dead downwind)</li>\n"
"<li><b>TWS</b> — true wind speed, in knots</li>\n"
"<li><b>STW / BSP</b> — boat speed through the water (log): what the polar represents</li>\n"
"<li><b>SOG</b> — speed over ground (GPS); used to make STW more reliable</li>\n"
"<li><b>VMG</b> — speed component along the wind axis: upwind and downwind efficiency</li>\n"
"<li><b>AWA / AWS</b> — apparent wind, as felt aboard</li>\n"
"</ul>\n"
"<h3>Getting started</h3>\n"
"<ol>\n"
"<li><b>Boat</b> card: pick a <b>recent</b> boat, or type a folder and <b>Open</b>, or create one with <b>New</b>. A boat = a folder holding its config and its polars.</li>\n"
"<li><b>Boat</b> tab: fill the inventory (mainsail, headsails, sea states) and define your <b>polars</b> with their criteria.</li>\n"
"<li>Feed it: <b>Data</b> tab → <b>Import files</b> (Create / Update), or the <b>Live</b> card while sailing.</li>\n"
"<li>Read the result (<b>Diagram</b>, <b>VMG</b>), fix values in <b>Data</b>, print to PDF with ⎙.</li>\n"
"</ol>\n"
"<h3>The tabs</h3>\n"
"<ul>\n"
"<li><b>Diagram</b> — one curve per TWS (range selectable). Colour marks the <b>useful VMG</b> range, <b>red</b> the degraded VMG (too tight upwind, too low downwind). <b>Dynamic mode</b>: enter any TWS and hover the diagram to read TWA / AWA / AWS / BS / VMG.</li>\n"
"<li><b>Data</b> — editable table: one row per TWA, one column per TWS. Edit cells, add or delete rows and columns, then <b>Save</b>.</li>\n"
"<li><b>VMG</b> — best upwind and downwind angles for each TWS.</li>\n"
"<li><b>Boat</b> — inventory and polars. For each polar, its criteria as checkboxes; <b>checking nothing = everything</b>.</li>\n"
"</ul>\n"
"<h3>Live capture</h3>\n"
"<p><b>Live</b> card: choose the source — <b>UDP</b> (listening port), <b>TCP</b> (host:port) or <b>qtVlm VDR</b> (path to the .db) or <b>Actisense NGX-1</b> (serial port: <code>/dev/ttyUSB0</code>, <code>COM3</code>, <code>@230400</code> to change the baud rate; gateway in Transfer mode) — then <b>Start</b>. Over UDP/TCP the format is detected automatically: <b>NMEA 0183</b> or <b>NMEA 2000</b> as YDRAW text (n2k-mux port 2700, Yacht Devices gateways).</p>\n"
"<p>Set the boat state live (mainsail, headsail, sea state): every point is routed to <b>all</b> the polars whose criteria match. The <b>Engine</b> button suspends recording while the propeller is engaged.</p>\n"
"<p>The grey cloud shows raw points, the red dot the current measurement, and the curve builds up as you sail. On <b>Stop</b>, every polar that received data is saved.</p>\n"
"<h3>How it is computed</h3>\n"
"<ul>\n"
"<li>points are grouped into 5° (TWA) and 2 kn (TWS) buckets, minimum 3 points per bucket</li>\n"
"<li>a <b>high percentile</b> is kept (P90 by default, adjustable P85–P95 in the <b>Data</b> tab): <i>achievable</i> performance, not the average</li>\n"
"<li>STW is smoothed then denoised against SOG (log spikes are rejected)</li>\n"
"<li>points under engine are excluded</li>\n"
"</ul>\n"
"<h3>File formats</h3>\n"
"<ul>\n"
"<li><b>NMEA</b> (.nmea, .log, .txt) — wind MWV or MWD, speed through water VHW, heading HDT/HDG; SOG read when present</li>\n<li><b>NMEA 2000</b> (YDRAW text log) — wind 130306, speed through water 128259, SOG 129026, heading 127250</li>\n"
"<li><b>VDR</b> (.db) — qtVlm SQLite database</li>\n"
"<li><b>Polar</b> (.pol) — semicolon-separated table</li>\n"
"</ul>\n"
"<h3>Tips</h3>\n"
"<ul>\n"
"<li>Combine several outings, in varied conditions.</li>\n"
"<li>An empty cell simply means: not enough data yet.</li>\n"
"<li><b>Update</b> re-aggregates existing and new data: the polar can rise <i>or</i> fall — handy to bring a theoretical VPP polar back to reality.</li>\n"
"</ul>\n"
"`};\n"
"$('#help').onclick=()=>{$('#hbody').innerHTML=HELP[lang]||HELP.fr;$('#hmodal').style.display='block';};\n"
"$('#logout').onclick=()=>{location.href='/logout';};\n"
"$('#hclose').onclick=()=>{$('#hmodal').style.display='none';};\n"
"$('#hmodal').onclick=e=>{if(e.target.id==='hmodal')$('#hmodal').style.display='none';};\n"
"addEventListener('keydown',e=>{if(e.key==='Escape')$('#hmodal').style.display='none';});\n"
"function renderVmg(){if(!P||!P.curves||!P.curves.length){$('#vmgtable').innerHTML='';return;}\n"
" let h='<table class=dt><tr><th>TWS</th><th>'+T('vmgup')+' °</th><th>BS</th><th>VMG</th><th>'+T('vmgdn')+' °</th><th>BS</th><th>VMG</th></tr>';\n"
" P.curves.forEach((c,k)=>{const au=c.a_up,ad=c.a_dn,bu=interpBS(c,au),bd=interpBS(c,ad);\n"
"  const vu=Math.abs(bu*Math.cos(au*Math.PI/180)),vd=Math.abs(bd*Math.cos(ad*Math.PI/180));\n"
"  h+='<tr><td>'+P.tws[k]+'</td><td>'+au.toFixed(0)+'</td><td>'+bu.toFixed(2)+'</td><td>'+vu.toFixed(2)+'</td><td>'+ad.toFixed(0)+'</td><td>'+bd.toFixed(2)+'</td><td>'+vd.toFixed(2)+'</td></tr>';});\n"
"  $('#vmgtable').innerHTML=h+'</table>';}\n"
"function applyTheme(){document.body.classList.toggle('light',theme==='light');$('#theme').textContent=theme==='dark'?'☀':'🌙';}\n"
"$('#lang').onclick=()=>{lang=lang==='fr'?'en':'fr';localStorage.setItem('lang',lang);$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();load();};\n"
"$('#theme').onclick=()=>{theme=theme==='dark'?'light':'dark';localStorage.setItem('theme',theme);applyTheme();draw();};\n"
"applyTheme();$('#lang').textContent=lang==='fr'?'EN':'FR';i18n();loadBoats();loadBoat();load();\n"
"pollLive().then(()=>{if(LIVE&&LIVE.on)startPoll();});\n"
"</script></body></html>\n";

/* Page de connexion : remplace le popup HTTP Basic. Langue et thème repris du
 * localStorage de la SPA (même origine). Erreur signalée par ?e=1 / ?e=2. */
static const char LOGIN[] =
"<!DOCTYPE html><html lang='fr'><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>Polar Doctor</title>\n"
"<style>\n"
":root{--bg:#0f1216;--fg:#d8dee5;--panel:#161b22;--border:#2a313a;--muted:#8b949e;--accent:#58a6ff;--inbg:#0b0e12;--active:#1f6feb}\n"
"body.light{--bg:#fff;--fg:#1b2430;--panel:#f3f5f8;--border:#cfd8e3;--muted:#5b6675;--accent:#0b62d6;--inbg:#fff}\n"
"*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg);display:flex;min-height:100vh;align-items:center;justify-content:center}\n"
"form{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:1.6em 1.8em;width:min(92vw,340px)}\n"
"h1{margin:0 0 .8em;font-size:1.25em;color:var(--accent)}\n"
"label{display:block;font-size:.85em;color:var(--muted);margin:.7em 0 .25em}\n"
"input{width:100%;padding:.5em .6em;background:var(--inbg);color:var(--fg);border:1px solid var(--border);border-radius:6px;font-size:1em}\n"
"button{margin-top:1.2em;width:100%;padding:.6em;background:var(--active);color:#fff;border:0;border-radius:6px;font-size:1em;cursor:pointer}\n"
"#err{color:#ff7b72;font-size:.85em;margin-top:.8em;min-height:1.2em}\n"
"</style></head><body>\n"
"<form method='post' action='/login'>\n"
"<h1>Polar Doctor</h1>\n"
"<label for='u' id='lu'>Utilisateur</label><input id='u' name='user' autocomplete='username' autofocus required>\n"
"<label for='p' id='lp'>Mot de passe</label><input id='p' name='pass' type='password' autocomplete='current-password' required>\n"
"<button id='bt'>Se connecter</button><div id='err'></div></form>\n"
"<script>\n"
"const g=id=>document.getElementById(id);\n"
"const lang=localStorage.getItem('lang')||((navigator.language||'fr').toLowerCase().startsWith('fr')?'fr':'en');\n"
"const theme=localStorage.getItem('theme')||((window.matchMedia&&matchMedia('(prefers-color-scheme: light)').matches)?'light':'dark');\n"
"document.body.classList.toggle('light',theme==='light');document.documentElement.lang=lang;\n"
"const E={fr:{u:'Utilisateur',p:'Mot de passe',b:'Se connecter',e1:'Identifiants incorrects.',e2:'Trop de tentatives : réessayez dans 30 s.'},\n"
"en:{u:'User',p:'Password',b:'Sign in',e1:'Wrong credentials.',e2:'Too many attempts: retry in 30 s.'}}[lang];\n"
"g('lu').textContent=E.u;g('lp').textContent=E.p;g('bt').textContent=E.b;\n"
"const q=new URLSearchParams(location.search).get('e');if(q)g('err').textContent=q==='2'?E.e2:E.e1;\n"
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

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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
    if (h > 0) sock_write(fd, hdr, (size_t)h);
    if (body && len) sock_write(fd, body, len);
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

/* 401 SANS en-tête WWW-Authenticate : c'est lui qui fait ouvrir le popup
 * d'identification du navigateur. La SPA renvoie alors vers la page de connexion. */
static void send_401(int fd)
{
    send_text(fd, 401, "Unauthorized", "application/json", "{\"ok\":false,\"err\":\"auth\"}");
}

static int authed(const char *req)
{
    if (!g_auth) return 1;
    /* Cookie de session posé par la page de connexion. */
    const char *c = strcasestr(req, "\r\nCookie:");
    if (c && g_token[0]) {
        const char *eol = strstr(c + 2, "\r\n");
        const char *t = strstr(c, "pd_auth=");
        if (t && (!eol || t < eol)) {
            t += 8; char tok[80]; size_t i = 0;
            while (t[i] && t[i] != ';' && t[i] != '\r' && t[i] != ' ' && i < sizeof tok - 1) { tok[i] = t[i]; i++; }
            tok[i] = '\0';
            if (ct_eq(tok, g_token)) return 1;
        }
    }
    /* Sinon HTTP Basic, pour curl -u et les scripts. */
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
    if (w < 0 || (size_t)w >= cap - n) return false; \
    n += (size_t)w; } while (0)
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
    const char *b = last_sep(path); b = b ? b + 1 : path;
    snprintf(out, cap, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && g_ascii_strcasecmp(dot, ".pol") == 0) *dot = '\0';
}

static void scan_boat(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const gchar *nm;
    while ((nm = g_dir_read_name(d)) && g_npol < MAXPOL) {
        size_t l = strlen(nm);
        if (l < 4 || g_ascii_strcasecmp(nm + l - 4, ".pol") != 0) continue;
        snprintf(g_pol_paths[g_npol], sizeof g_pol_paths[0], "%s/%s", dir, nm);
        base_no_ext(nm, g_pol_names[g_npol], sizeof g_pol_names[0]);
        g_npol++;
    }
    g_dir_close(d);
    for (int i = 0; i < g_npol - 1; i++)            /* tri alphabétique */
        for (int j = i + 1; j < g_npol; j++)
            if (g_ascii_strcasecmp(g_pol_names[i], g_pol_names[j]) > 0) {
                char tp[sizeof g_pol_paths[0]], tn[sizeof g_pol_names[0]];   /* échange de lignes entières */
                memcpy(tp, g_pol_paths[i], sizeof tp); memcpy(tn, g_pol_names[i], sizeof tn);
                memcpy(g_pol_paths[i], g_pol_paths[j], sizeof tp); memcpy(g_pol_names[i], g_pol_names[j], sizeof tn);
                memcpy(g_pol_paths[j], tp, sizeof tp); memcpy(g_pol_names[j], tn, sizeof tn);
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

/* Ouvre un dossier-bateau : config + inventaire, liste des .pol, 1re polaire,
 * et mémorise le bateau dans les récents. Réutilisé au démarrage et par /api/open. */
static bool open_boat_dir(const char *folder)
{
    char dir[512]; snprintf(dir, sizeof dir, "%s", folder);
    size_t L = strlen(dir); while (L > 1 && (dir[L - 1] == '/' || dir[L - 1] == '\\')) dir[--L] = '\0';
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) return false;

    boat_config_init(&g_boat_config);           /* repart d'un inventaire propre */
    const char *b = last_sep(dir);
    snprintf(g_boat_name, sizeof g_boat_name, "%s", b ? b + 1 : dir);

    char cfg[BOAT_PATH_LEN];
    if (boat_find_config(dir, cfg, sizeof cfg) && boat_config_load(&g_boat_config, cfg)) {
        snprintf(g_boat_config_path, BOAT_PATH_LEN, "%s", cfg);
        if (g_boat_config.name[0]) snprintf(g_boat_name, sizeof g_boat_name, "%s", g_boat_config.name);
    } else g_boat_config_path[0] = '\0';

    snprintf(g_boat_dir, sizeof g_boat_dir, "%s", dir);
    g_npol = 0; g_cur = 0; g_loaded = 0;
    scan_boat(dir);
    if (g_npol > 0 && load_polar_file(g_pol_paths[0], &g_polar)) { g_loaded = 1; g_cur = 0; }
    boat_recent_add(dir);
    return true;
}

/* GET /api/boats : bateaux récents + dossier courant (pour le menu « Ouvrir »). */
static void serve_boats(int fd)
{
    static char list[BOAT_RECENT_MAX][BOAT_PATH_LEN];
    int n = boat_recent_load(list, BOAT_RECENT_MAX);
    static char buf[8192]; size_t o = 0; int w;
#define APP(...) do { w = snprintf(buf + o, sizeof buf - o, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - o) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    o += (size_t)w; } while (0)
    char e[BOAT_PATH_LEN * 2];
    json_escape(g_boat_dir, e, sizeof e);
    APP("{\"auth\":%s,\"current\":\"%s\",\"recent\":[", g_auth ? "true" : "false", e);
    for (int i = 0; i < n; i++) { json_escape(list[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); }
    APP("]}");
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* GET /api/newboat?folder=&name= : crée le dossier + boat.cfg, puis l'ouvre. */
static void serve_newboat(int fd, const char *folder, const char *name)
{
    if (!folder[0]) { send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}"); return; }
    g_mkdir(folder, 0755);                       /* ignore EEXIST */
    if (!g_file_test(folder, G_FILE_TEST_IS_DIR)) { send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}"); return; }
    BoatConfig c; boat_config_init(&c);
    snprintf(c.name, sizeof c.name, "%s", name[0] ? name : "Bateau");
    char cfg[700]; snprintf(cfg, sizeof cfg, "%s/boat.cfg", folder);
    if (!boat_config_save(&c, cfg) || !open_boat_dir(folder)) { send_text(fd, 500, "Error", "application/json", "{\"ok\":false}"); return; }
    send_text(fd, 200, "OK", "application/json", "{\"ok\":true}");
}

/* GET /api/config : la configuration bateau complète en JSON (pour l'éditeur). */
static void serve_config_get(int fd)
{
    static char buf[16384]; size_t o = 0; int w;
#define APP(...) do { w = snprintf(buf + o, sizeof buf - o, __VA_ARGS__); \
    if (w < 0 || (size_t)w >= sizeof buf - o) { send_text(fd, 500, "Error", "application/json", "{}"); return; } \
    o += (size_t)w; } while (0)
#define ARR(field, count) do { APP("["); \
    for (int i = 0; i < (count); i++) { json_escape((field)[i], e, sizeof e); APP("%s\"%s\"", i ? "," : "", e); } \
    APP("]"); } while (0)
    char e[BOAT_TERM_LEN * 2 + 8];
    const BoatConfig *c = &g_boat_config;
    json_escape(c->name, e, sizeof e); APP("{\"name\":\"%s\",\"mains\":", e);
    ARR(c->mainsail, c->n_mainsail);
    APP(",\"heads\":");  ARR(c->headsail, c->n_headsail);
    APP(",\"seas\":");   ARR(c->seastate, c->n_seastate);
    json_escape(c->kw_moteur, e, sizeof e); APP(",\"moteur\":\"%s\"", e);
    json_escape(c->kw_charge, e, sizeof e); APP(",\"charge\":\"%s\",\"polars\":[", e);
    for (int k = 0; k < c->n_polars; k++) {
        const PolarDef *p = &c->polars[k];
        json_escape(p->name, e, sizeof e);
        APP("%s{\"name\":\"%s\",\"mains\":", k ? "," : "", e);
        ARR(p->mains, p->n_mains);
        APP(",\"heads\":"); ARR(p->heads, p->n_heads);
        APP(",\"seas\":");  ARR(p->seas,  p->n_seas);
        APP("}");
    }
    APP("]}");
#undef ARR
#undef APP
    send_text(fd, 200, "OK", "application/json", buf);
}

/* POST /api/config : corps = texte INI du boat.cfg (généré par le formulaire).
 * Écrit un .tmp, valide en le rechargeant, puis remplace et recharge la config. */
static void serve_config_post(int fd, char *body)
{
    if (!g_boat_dir[0]) { send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false,\"err\":\"aucun bateau ouvert\"}"); return; }
    char target[BOAT_PATH_LEN];
    int tl = g_boat_config_path[0] ? snprintf(target, sizeof target, "%s", g_boat_config_path)
                                   : snprintf(target, sizeof target, "%s/boat.cfg", g_boat_dir);
    if (tl < 0 || (size_t)tl >= sizeof target) { send_text(fd, 500, "Error", "application/json", "{\"ok\":false,\"err\":\"chemin trop long\"}"); return; }
    char tmp[BOAT_PATH_LEN + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", target);

    int fdw = g_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fdw < 0) { send_text(fd, 500, "Error", "application/json", "{\"ok\":false}"); return; }
    size_t len = strlen(body), w = 0;
    while (w < len) { ssize_t x = write(fdw, body + w, len - w); if (x <= 0) break; w += (size_t)x; }
    close(fdw);

    BoatConfig test; boat_config_init(&test);
    if (boat_config_load(&test, tmp) && test.name[0] && replace_file(tmp, target) == 0) {
        g_boat_config = test;
        snprintf(g_boat_config_path, BOAT_PATH_LEN, "%s", target);
        snprintf(g_boat_name, sizeof g_boat_name, "%s", test.name);
        send_text(fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else {
        g_unlink(tmp);
        send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}");
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
 * Sources : TCP (client) et UDP (écoute) en NMEA 0183 ou N2K YDRAW, VDR qtVlm,
 * passerelle série Actisense NGX-1/NGT-1. État exposé en polling. */

static int    g_live_on = 0, g_live_src = 0, g_live_fd = -1;  /* src 1=tcp 2=udp 3=vdr 4=ngx (série) */
static char   g_live_err[200] = "";       /* cause du dernier échec de démarrage / de la perte de source */
static actisense_rx_t g_act;                /* décodeur série Actisense */
static time_t g_act_ping = 0;               /* dernier envoi de la commande « tous les PGN » */
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
static char   g_live_saved[700] = "";      /* chemin du .pol écrit au dernier arrêt */

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
    if (twa < 0 || twa > 180 || tws < 0 || tws > 70 || bsp < 0 || bsp > 50) return;  /* = filtre add_data_point */
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

/* Un point TWA+TWS+STW complet dans g_lnmea : même pipeline que process_nmea_file (lissé). */
static void live_point(void)
{
    if (g_lnmea.has_sog && !stw_sog_accept(&g_lfilt, g_lnmea.bsp, g_lnmea.sog)) return;
    double twa = g_lnmea.twa, tws = g_lnmea.tws, bsp = g_lnmea.bsp;
    if (NMEA_SMOOTH_WINDOW > 1)
        nmea_smoother_push(&g_lsm, g_lnmea.twa, g_lnmea.tws, g_lnmea.bsp, &twa, &tws, &bsp);
    live_add(twa, tws, bsp);
}

static void live_feed_sentence(const char *line)
{
    if (parse_nav_line(line, &g_lnmea)) live_point();   /* NMEA 0183 ou N2K YDRAW */
}

/* Passerelle Actisense : relève le port série, décode, et renvoie toutes les 20 s
 * la commande « tous les PGN » (comme canboat/actisense-serial). */
static void live_serial_tick(void)
{
    uint8_t b[4096];
    long got;
    while ((got = serial_read(b, sizeof b)) > 0)
        for (long i = 0; i < got; i++) {
            int pgn, len; const uint8_t *data;
            if (actisense_rx_byte(&g_act, b[i], &pgn, &data, &len) && n2k_apply_frame(pgn, data, len, &g_lnmea))
                live_point();
        }
    if (got < 0) {
        snprintf(g_live_err, sizeof g_live_err, "port série perdu (%s)", g_live_addr);
        serial_close(); g_live_on = 0;
        return;
    }
    if (time(NULL) - g_act_ping >= 20) {
        uint8_t st[32]; size_t n = actisense_startup_frame(st, sizeof st);
        serial_write(st, n); g_act_ping = time(NULL);
    }
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
    int fd = (int)socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) { sock_close(fd); fd = -1; }
    freeaddrinfo(res);
    return fd;
}

static int open_udp(const char *addr)
{
    const char *c = strrchr(addr, ':');
    int port = atoi(c ? c + 1 : addr);
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { sock_close(fd); return -1; }
    return fd;
}

static void live_stop(void)
{
    if (g_live_fd >= 0) sock_close(g_live_fd);
    g_live_fd = -1;
    if (g_vdr) { sqlite3_close(g_vdr); g_vdr = NULL; }
    serial_close();
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
    g_live_err[0] = 0;
    if (src == 3) {
        ok = (live_vdr_open(addr) == 0);
        if (!ok) snprintf(g_live_err, sizeof g_live_err, "VDR illisible : %s", addr);
    } else if (src == 4) {
        ok = serial_open(addr, g_live_err, sizeof g_live_err);
        if (ok) {
            actisense_rx_reset(&g_act);
            uint8_t st[32]; size_t n = actisense_startup_frame(st, sizeof st);
            serial_write(st, n); g_act_ping = time(NULL);
        }
    } else {
        int fd = (src == 1) ? open_tcp(addr) : open_udp(addr);
        if (fd >= 0) g_live_fd = fd;
        ok = (fd >= 0);
        if (!ok) snprintf(g_live_err, sizeof g_live_err, "%s : %s", src == 1 ? "connexion TCP impossible" : "écoute UDP impossible", addr);
    }
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
            char *sl = last_sep(dir); if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
        }
        time_t t = time(NULL); struct tm tmv; local_tm(t, &tmv);
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
    { char e[400]; json_escape(g_live_err, e, sizeof e); APP("\"err\":\"%s\",", e); }
    {   /* VDR qtVlm proposé par défaut : ~/.qtVlm/vdrs/vdr.db de l'utilisateur du
         * serveur, seulement s'il existe (emplacement Windows non documenté : on ne devine pas). */
        char *vd = g_build_filename(g_get_home_dir(), ".qtVlm", "vdrs", "vdr.db", NULL);
        char e[1100]; json_escape(g_file_test(vd, G_FILE_TEST_IS_REGULAR) ? vd : "", e, sizeof e);
        g_free(vd);
        APP("\"vdr_default\":\"%s\",", e);
    }
    if (g_cur_twa >= 0 && g_cur_bsp > 0) APP("\"cur\":[%.1f,%.2f,%.1f],", g_cur_twa, g_cur_bsp, g_cur_tws);
    else APP("\"cur\":null,");
    APP("\"pts\":[");
    int start = (g_lpt_head - g_lpt_n + LIVE_PTS) % LIVE_PTS;
    for (int k = 0; k < g_lpt_n; k++) {
        int idx = (start + k) % LIVE_PTS;
        APP("%s[%.1f,%.2f]", k ? "," : "", g_lpt[idx][0], g_lpt[idx][1]);
    }
    { char e[1400]; json_escape(g_live_saved, e, sizeof e);
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
    int fdw = g_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fdw < 0) { send_text(fd, 500, "Error", "application/json", "{\"ok\":false}"); return; }
    size_t len = strlen(body), w = 0;
    while (w < len) { ssize_t x = write(fdw, body + w, len - w); if (x <= 0) break; w += (size_t)x; }
    close(fdw);
    PolarData test; init_polar_data(&test);
    /* load_polar_file est laxiste : on exige une polaire non dégénérée (>=1 TWA,
     * >=2 colonnes TWS dont la sentinelle 0) avant de remplacer le fichier. */
    if (load_polar_file(tmp, &test) && test.num_angles >= 1 && test.num_speeds >= 2
        && replace_file(tmp, g_pol_paths[g_cur]) == 0) {
        g_polar = test;
        snprintf(g_polar.filename, sizeof g_polar.filename, "%s", g_pol_paths[g_cur]);
        g_loaded = 1;
        send_text(fd, 200, "OK", "application/json", "{\"ok\":true}");
    } else {
        g_unlink(tmp);
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
        else if (g_npol > 0) { snprintf(dir, sizeof dir, "%s", g_pol_paths[0]); char *s = last_sep(dir); if (s) *s = 0; else snprintf(dir, sizeof dir, "."); }
        time_t t = time(NULL); struct tm tmv; local_tm(t, &tmv);
        char ts[32]; strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);
        snprintf(path, sizeof path, "%s/import_%s.pol", dir, ts);
    }

    int ok = (lp.num_angles >= 1 && lp.num_speeds >= 2 && save_polar_file(path, &lp));
    if (ok) {
        PolarData t; init_polar_data(&t);
        if (load_polar_file(path, &t)) { g_polar = t; snprintf(g_polar.filename, sizeof g_polar.filename, "%.*s", (int)sizeof g_polar.filename - 1, path); g_loaded = 1; }
        rescan_boat();
        for (int i = 0; i < g_npol; i++) if (strcmp(g_pol_paths[i], path) == 0) { g_cur = i; break; }
    }
    free_polar_grid(&g);

    char e[700], out[900]; json_escape(ok ? path : "", e, sizeof e);
    snprintf(out, sizeof out, "{\"ok\":%s,\"files\":%d,\"points\":%d,\"saved\":\"%s\"}",
             ok ? "true" : "false", files, total, e);
    send_text(fd, ok ? 200 : 400, ok ? "OK" : "Bad Request", "application/json", out);
}

/* 303 vers `loc`, avec éventuellement un Set-Cookie. */
static void send_redirect(int fd, const char *loc, const char *cookie)
{
    char h[512];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 303 See Other\r\nLocation: %s\r\n%s%s%s"
        "Content-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        loc, cookie ? "Set-Cookie: " : "", cookie ? cookie : "", cookie ? "\r\n" : "");
    if (n > 0) sock_write(fd, h, (size_t)n);
}

/* POST /login (formulaire user/pass). Anti-force-brute NON bloquant : après 5
 * échecs, refus pendant 30 s (un sleep gèlerait la boucle poll() et la capture). */
static void handle_login(int fd, const char *body)
{
    static int fails = 0;
    static time_t lock_until = 0;
    time_t now = time(NULL);
    if (now < lock_until) { send_redirect(fd, "/?e=2", NULL); return; }

    char u[128] = "", p[256] = "", cred[400];
    const char *pu = strstr(body, "user="), *pp = strstr(body, "pass=");
    if (pu) url_decode(pu + 5, u, sizeof u);
    if (pp) url_decode(pp + 5, p, sizeof p);
    snprintf(cred, sizeof cred, "%s:%s", u, p);

    if (g_auth && g_token[0] && ct_eq(cred, g_auth)) {
        fails = 0;
        char ck[200];
        snprintf(ck, sizeof ck, "pd_auth=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict", g_token);
        send_redirect(fd, "/", ck);
    } else {
        if (++fails >= 5) { lock_until = now + 30; fails = 0; }
        send_redirect(fd, "/?e=1", NULL);
    }
}

/* Jeton de session = HMAC-SHA256(secret serveur, "user:pass"). Le secret est
 * conservé dans ~/.config/polar_doctor/web_secret (0600) : les cookies survivent
 * aux redémarrages et deviennent invalides dès que le mot de passe change.
 * Supprimer ce fichier déconnecte tous les appareils. */
static bool init_session_token(void)
{
    char secret[65] = "";
    char *dir = g_build_filename(g_get_user_config_dir(), "polar_doctor", NULL);
    char *path = g_build_filename(dir, "web_secret", NULL);
    FILE *f = g_fopen(path, "r");
    if (f) { if (!fgets(secret, sizeof secret, f)) secret[0] = 0; fclose(f); secret[strcspn(secret, "\r\n")] = 0; }
    if (strlen(secret) < 64) {
        unsigned char rnd[32];
        if (!rand_bytes(rnd, sizeof rnd)) { fprintf(stderr, "polar_doctor_web : générateur aléatoire indisponible\n"); g_free(path); g_free(dir); return false; }
        for (int i = 0; i < 32; i++) snprintf(secret + 2 * i, 3, "%02x", rnd[i]);
        g_mkdir_with_parents(dir, 0700);
        int wfd = g_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
        if (wfd >= 0) { ssize_t w = write(wfd, secret, 64); (void)w; close(wfd); }
        else fprintf(stderr, "polar_doctor_web : secret non enregistré (%s) : reconnexion à chaque redémarrage\n", path);
    }
    gchar *h = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret), g_auth, -1);
    snprintf(g_token, sizeof g_token, "%s", h ? h : "");
    g_free(h); g_free(path); g_free(dir);
    return g_token[0] != 0;
}

/* ------------------------------------------------------------- web.conf --- */
/* Fichier de réglages du serveur, surtout pour Windows (pas de systemd ni de
 * /etc/default) : <config utilisateur>/polar_doctor/web.conf, soit
 * %LOCALAPPDATA%\polar_doctor\web.conf sous Windows et ~/.config/polar_doctor/web.conf
 * sous Linux, ou --config FICHIER. Format de /etc/default : CLÉ=valeur, # = commentaire.
 * Priorité : ligne de commande > environnement > web.conf. */
static char g_conf_auth[AUTH_RAW_MAX + 2];
static char g_conf_bind[64];
static char g_conf_boat[BOAT_PATH_LEN];
static int  g_conf_port;

#ifdef _WIN32
static const char CONF_TEMPLATE[] =
    "# Réglages de polar_doctor_web (une ligne CLÉ=valeur, # = commentaire).\n"
    "# Priorité : ligne de commande > variables d'environnement > ce fichier.\n"
    "\n"
    "# Identifiants utilisateur:motdepasse. OBLIGATOIRES pour écouter sur le réseau.\n"
    "#WEB_AUTH=moi:MonMotDePasse\n"
    "\n"
    "# Adresse d'écoute : 127.0.0.1 = ce PC seulement, 0.0.0.0 = tout le réseau.\n"
    "#BIND=0.0.0.0\n"
    "\n"
    "# Port d'écoute (défaut 8081).\n"
    "#PORT=8081\n"
    "\n"
    "# Bateau ouvert au démarrage (défaut : le plus récent).\n"
    "#POLAR_DOCTOR_BOAT=C:\\Bateaux\\MonBateau\n";
#endif

/* Lit web.conf. false = fichier illisible ou invalide (message déjà affiché).
 * Absent : true, et sous Windows un modèle commenté est créé pour qu'on le trouve. */
static bool load_web_conf(const char *path, bool explicit_path)
{
    gchar *txt = NULL; gsize len = 0;
    if (!g_file_get_contents(path, &txt, &len, NULL)) {
        if (explicit_path) { fprintf(stderr, "polar_doctor_web : impossible de lire %s\n", path); return false; }
#ifdef _WIN32
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
            gchar *dir = g_path_get_dirname(path);
            g_mkdir_with_parents(dir, 0700); g_free(dir);
            if (g_file_set_contents(path, CONF_TEMPLATE, -1, NULL))
                fprintf(stderr, "polar_doctor_web : modèle de réglages créé : %s\n", path);
        }
#endif
        return true;
    }
    bool ok = true, has_auth = false;
    int ln = 0;
    for (char *line = txt, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        ln++;
        g_strstrip(line);                                   /* espaces, \r (CRLF), tabulations */
        if (ln == 1 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            memmove(line, line + 3, strlen(line + 3) + 1); /* BOM UTF-8 (Bloc-notes) */
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { fprintf(stderr, "%s:%d : ligne ignorée (CLÉ=valeur attendu)\n", path, ln); continue; }
        *eq = 0;
        char *key = g_strstrip(line), *val = g_strstrip(eq + 1);
        size_t vl = strlen(val);
        if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) { val[vl - 1] = 0; val++; }
        if (strcmp(key, "WEB_AUTH") == 0 || strcmp(key, "POLAR_DOCTOR_WEB_AUTH") == 0) {
            if (strlen(val) > AUTH_RAW_MAX) { fprintf(stderr, "%s:%d : WEB_AUTH trop long (max %d)\n", path, ln, AUTH_RAW_MAX); ok = false; }
            else { snprintf(g_conf_auth, sizeof g_conf_auth, "%s", val); has_auth = val[0] != 0; }
        } else if (strcmp(key, "BIND") == 0) {
            snprintf(g_conf_bind, sizeof g_conf_bind, "%s", val);
        } else if (strcmp(key, "PORT") == 0) {
            char *end; long p = strtol(val, &end, 10);
            if (!val[0] || *end || p < 1 || p > 65535) { fprintf(stderr, "%s:%d : PORT invalide : %s\n", path, ln, val); ok = false; }
            else g_conf_port = (int)p;
        } else if (strcmp(key, "POLAR_DOCTOR_BOAT") == 0) {
            snprintf(g_conf_boat, sizeof g_conf_boat, "%s", val);
        } else {
            fprintf(stderr, "%s:%d : clé inconnue ignorée : %s\n", path, ln, key);
        }
    }
    g_free(txt);
#ifndef _WIN32
    /* Le fichier porte un mot de passe : même exigence que ssh sur ses clés. */
    struct stat st;
    if (has_auth && g_stat(path, &st) == 0 && (st.st_mode & 077)) {
        fprintf(stderr, "polar_doctor_web : %s contient WEB_AUTH mais est lisible par d'autres "
                        "utilisateurs : chmod 600 %s\n", path, path);
        ok = false;
    }
#else
    (void)has_auth;
#endif
    if (ok) fprintf(stderr, "polar_doctor_web : réglages lus dans %s\n", path);
    return ok;
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

    /* Accès : page de connexion / cookie de session (HTTP Basic accepté pour curl). */
    int is_page = (strcmp(path, "/") == 0 || strncmp(path, "/?", 2) == 0);
    if (strcmp(path, "/logout") == 0) {
        send_redirect(fd, "/", "pd_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/login") == 0) { handle_login(fd, body ? body : ""); return; }
    if (!authed(req)) {
        if (strcmp(method, "GET") == 0 && is_page)
            send_resp(fd, 200, "OK", "text/html; charset=utf-8", LOGIN, sizeof LOGIN - 1);
        else send_401(fd);
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/save") == 0 && body) serve_save(fd, body);
        else if (strcmp(path, "/api/config") == 0 && body) serve_config_post(fd, body);
        else if (strncmp(path, "/api/import", 11) == 0 && body)
            serve_import(fd, body, strstr(path, "update") != NULL);
        else send_text(fd, 404, "Not Found", "text/plain", "404\n");
        return;
    }
    if (strcmp(method, "GET") != 0) { send_text(fd, 400, "Bad Request", "text/plain", "400\n"); return; }

    if (is_page)
        send_resp(fd, 200, "OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
    else if (strcmp(path, "/api/polar") == 0)
        serve_polar(fd);
    else if (strncmp(path, "/api/curve", 10) == 0) {
        const char *q = strstr(path, "tws=");
        serve_curve(fd, q ? atof(q + 4) : 0);
    }
    else if (strcmp(path, "/api/boat") == 0)
        serve_boat(fd);
    else if (strcmp(path, "/api/boats") == 0)
        serve_boats(fd);
    else if (strncmp(path, "/api/percentile", 15) == 0) {   /* ?p=85..95 : règle, sinon lit */
        const char *q = strstr(path, "p=");
        if (q) { int p = atoi(q + 2); if (p >= 85 && p <= 95) g_polar_percentile = p; }
        char o[32]; snprintf(o, sizeof o, "{\"p\":%d}", g_polar_percentile);
        send_text(fd, 200, "OK", "application/json", o);
    }
    else if (strcmp(path, "/api/config") == 0)
        serve_config_get(fd);
    else if (strncmp(path, "/api/open", 9) == 0) {
        const char *q = strstr(path, "folder=");
        char f[BOAT_PATH_LEN] = "";
        if (q) url_decode(q + 7, f, sizeof f);
        if (f[0] && open_boat_dir(f)) serve_boat(fd);
        else send_text(fd, 400, "Bad Request", "application/json", "{\"ok\":false}");
    }
    else if (strncmp(path, "/api/newboat", 12) == 0) {
        const char *qf = strstr(path, "folder="), *qn = strstr(path, "name=");
        char f[BOAT_PATH_LEN] = "", nm[128] = "";
        if (qf) url_decode(qf + 7, f, sizeof f);
        if (qn) url_decode(qn + 5, nm, sizeof nm);
        serve_newboat(fd, f, nm);
    }
    else if (strncmp(path, "/api/select", 11) == 0) {
        const char *q = strstr(path, "i=");
        serve_select(fd, q ? atoi(q + 2) : -1);
    }
    else if (strncmp(path, "/api/live/start", 15) == 0) {
        const char *ps = strstr(path, "src="), *pa = strstr(path, "addr=");
        int src = 2;
        if (ps) { if (strncmp(ps + 4, "tcp", 3) == 0) src = 1; else if (strncmp(ps + 4, "vdr", 3) == 0) src = 3;
                  else if (strncmp(ps + 4, "ngx", 3) == 0) src = 4; }
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
    int port = 0;                  /* 0 = non fixé : web.conf, sinon 8081 (8080 = n2k-mux-web) */
    const char *bind_addr = NULL;  /* NULL = non fixé : web.conf, sinon 127.0.0.1 */
    const char *pol = NULL;
    const char *conf_path = NULL;  /* --config FICHIER */
    int allow_anon = 0;   /* --allow-anonymous : écoute réseau sans auth, assumée */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_addr = argv[++i];
        else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) g_auth = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) conf_path = argv[++i];
        else if (strcmp(argv[i], "--allow-anonymous") == 0) allow_anon = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            gchar *def_conf = g_build_filename(g_get_user_config_dir(), "polar_doctor", "web.conf", NULL);
            fprintf(stderr,
                "Usage : %s [fichier.pol | dossier-bateau] [--port N] [--bind ADDR] [--config FICHIER]\n"
                "  --port N     port d'écoute (défaut 8081 ; 8080 = n2k-mux-web)\n"
                "  --bind ADDR  adresse d'écoute (défaut 127.0.0.1 ; 0.0.0.0 = LAN)\n"
                "  --auth u:p   authentification HTTP Basic (préférer l'environnement :\n"
                "               POLAR_DOCTOR_WEB_AUTH ou WEB_AUTH — argv est lisible via /proc)\n"
                "  --config F   fichier de réglages (défaut : %s)\n"
                "  --allow-anonymous  autorise l'écoute réseau SANS authentification\n"
                "  Sans chemin : POLAR_DOCTOR_BOAT, sinon le bateau le plus récent.\n",
                argv[0], def_conf);
            g_free(def_conf);
            return 0;
        } else if (argv[i][0] != '-') pol = argv[i];
        else { fprintf(stderr, "option inconnue : %s\n", argv[i]); return 2; }
    }

    /* Réglages : ligne de commande > environnement > web.conf > défauts. */
    {
        gchar *def_conf = g_build_filename(g_get_user_config_dir(), "polar_doctor", "web.conf", NULL);
        bool ok = load_web_conf(conf_path ? conf_path : def_conf, conf_path != NULL);
        g_free(def_conf);
        if (!ok) return 2;
    }
    if (!port) port = g_conf_port ? g_conf_port : 8081;
    if (!bind_addr) bind_addr = g_conf_bind[0] ? g_conf_bind : "127.0.0.1";

    /* Le credential peut venir de l'environnement ou de web.conf plutôt que de la
     * ligne de commande : argv est lisible par tout utilisateur local via /proc. */
    if (!g_auth) {
        const char *env = getenv("POLAR_DOCTOR_WEB_AUTH");
        if (!env || !env[0]) env = getenv("WEB_AUTH");   /* /etc/default/polar_doctor_web */
        if (env && env[0]) g_auth = env;
        else if (g_conf_auth[0]) g_auth = g_conf_auth;
    }
    if (g_auth) {
        if (!strchr(g_auth, ':')) { fprintf(stderr, "--auth attend le format user:pass\n"); return 2; }
        if (strlen(g_auth) > AUTH_RAW_MAX) {
            fprintf(stderr, "--auth : credential trop long (max %d caractères)\n", AUTH_RAW_MAX); return 2;
        }
        b64encode(g_auth, g_auth_b64, sizeof g_auth_b64);
    }

    /* L'interface ÉCRIT polaires et boat.cfg, crée des dossiers et lit des chemins
     * serveur (/api/import) : l'exposer hors de la boucle locale sans
     * authentification donne ce pouvoir à tout le réseau. On refuse, sauf demande
     * explicite (--allow-anonymous). Même règle que n2k-mux-web. */
    if (!g_auth && !allow_anon && strncmp(bind_addr, "127.", 4) != 0 &&
        strcmp(bind_addr, "::1") != 0 && strcmp(bind_addr, "localhost") != 0) {
        fprintf(stderr,
            "polar_doctor_web : refus d'écouter sur %s sans authentification.\n"
#ifdef _WIN32
            "  Poser WEB_AUTH=user:pass dans %%LOCALAPPDATA%%\\polar_doctor\\web.conf,\n"
#else
            "  Poser WEB_AUTH=user:pass dans /etc/default/polar_doctor_web (service)\n"
            "  ou ~/.config/polar_doctor/web.conf,\n"
#endif
            "  ou --bind 127.0.0.1, ou --allow-anonymous pour assumer le risque.\n",
            bind_addr);
        return 2;
    }

    if (g_auth && !init_session_token()) return 1;

    /* Sans chemin : POLAR_DOCTOR_BOAT, sinon le bateau le plus récent (service
     * systemd sans configuration : il rouvre le dernier bateau utilisé). */
    static char recent0[BOAT_RECENT_MAX][BOAT_PATH_LEN];
    if (!pol) {
        const char *eb = getenv("POLAR_DOCTOR_BOAT");
        if (eb && eb[0]) pol = eb;
        else if (g_conf_boat[0]) pol = g_conf_boat;
        else if (boat_recent_load(recent0, BOAT_RECENT_MAX) > 0) pol = recent0[0];
    }

    init_polar_data(&g_polar);
    if (pol && g_file_test(pol, G_FILE_TEST_IS_DIR)) {
        /* dossier-bateau : config + inventaire + liste des .pol (cf. open_boat_dir) */
        if (!open_boat_dir(pol) || g_npol == 0)
            fprintf(stderr, "polar_doctor_web : aucune polaire chargeable dans %s\n", pol);
    } else if (pol) {
        if (load_polar_file(pol, &g_polar)) g_loaded = 1;
        else fprintf(stderr, "polar_doctor_web : impossible de charger %s\n", pol);
        snprintf(g_pol_paths[0], sizeof g_pol_paths[0], "%s", pol);
        base_no_ext(pol, g_pol_names[0], sizeof g_pol_names[0]);
        g_npol = 1; g_cur = 0;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "polar_doctor_web : WSAStartup a échoué\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    int ls = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
#ifndef _WIN32   /* sous Windows, SO_REUSEADDR permettrait à un autre processus de voler le port */
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#else
    (void)one;
#endif
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
        int r = poll(pfds, nf, !g_live_on ? -1 : g_live_src == 4 ? 50 : 1000);  /* série : relevé toutes les 50 ms */
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (pfds[0].revents & POLLIN) {
            int fd = (int)accept(ls, NULL, NULL);
            if (fd >= 0) {
                sock_timeouts(fd, 5);
                handle_client(fd);
                sock_close(fd);
            }
        }
        if (g_live_on && g_live_fd >= 0 && nf > 1 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char b[4096];
            ssize_t got = recv(g_live_fd, b, sizeof b, 0);
            if (got > 0) live_feed(b, (size_t)got);
            else if (got == 0 && g_live_src == 1) live_stop();  /* TCP fermé par la passerelle */
        }
        if (g_live_on && g_live_src == 3) live_vdr_tick();      /* tail VDR (~1/s) */
        if (g_live_on && g_live_src == 4) live_serial_tick();   /* passerelle Actisense */
    }
    sock_close(ls);
    return 0;
}
