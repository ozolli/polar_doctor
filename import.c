#include "libpolar.h"

/* Progression abstraite : relaie un message à l'UI si un callback est fourni. */
static void prog(ProgressContext *p, const char *msg) {
    if (p && p->update) p->update(p->ud, msg);
}

// Filtre de débruitage STW (offset STW-SOG suivi par EMA)
void stw_sog_reset(stw_sog_filter_t *f) { f->have_offset = false; }

// true = STW plausible (à garder) ; false = saut anormal (à rejeter). Met à jour
// l'offset sur les points acceptés ; le premier point amorce l'offset.
bool stw_sog_accept(stw_sog_filter_t *f, double stw, double sog) {
    double inst = stw - sog;
    if (!f->have_offset) { f->offset = inst; f->have_offset = true; return true; }
    if (fabs(inst - f->offset) > STW_SOG_TOL) return false;
    f->offset = STW_SOG_ALPHA * inst + (1.0 - STW_SOG_ALPHA) * f->offset;
    return true;
}

void init_polar_grid(polar_grid_t *grid) {
    memset(grid, 0, sizeof(polar_grid_t));
    grid->angle_min = 180;
    grid->speed_min = 100;
    grid->cache_valid = false;
}

void free_polar_grid(polar_grid_t *grid) {
    for (int i = 0; i < PG_MAX_ANGLES; i++) {
        for (int j = 0; j < PG_MAX_SPEEDS; j++) {
            data_point_t *p = grid->points[i][j];
            while (p) {
                data_point_t *next = p->next;
                free(p);
                p = next;
            }
        }
    }
}

bool verify_checksum(const char *sentence) {
    if (sentence[0] != '$') return false;
    const char *star = strchr(sentence, '*');
    if (!star) return true;
    unsigned char calc = 0;
    for (const char *p = sentence + 1; p < star; p++) calc ^= (unsigned char)*p;
    unsigned int expected;
    if (sscanf(star + 1, "%02X", &expected) != 1)
        if (sscanf(star + 1, "%02x", &expected) != 1) return false;
    return calc == (unsigned char)expected;
}

int round_to_bucket(double value, int step) {
    return (int)(round(value / step) * step);
}

// Découpe une trame sur ',' en PRÉSERVANT les champs vides (contrairement à
// strtok qui fusionne les délimiteurs). Modifie s en place. Renvoie le nb de champs.
int nmea_split(char *s, char **out, int maxf) {
    int n = 0;
    if (maxf <= 0) return 0;
    out[n++] = s;
    for (char *q = s; *q && n < maxf; q++)
        if (*q == ',') { *q = '\0'; out[n++] = q + 1; }
    return n;
}

// Lit le champ idx comme nombre. false si index hors borne ou champ vide.
bool nmea_field_num(char **f, int nf, int idx, double *out) {
    if (idx < 0 || idx >= nf || !f[idx] || f[idx][0] == '\0') return false;
    *out = atof(f[idx]);
    return true;
}

// Extrait le SOG (nœuds) des trames qui le portent. Met à jour data->sog/has_sog.
// Découpage robuste aux champs vides (fréquents sur les trames GPS).
void parse_sog_sentence(const char *type, char **f, int nf, nmea_data_t *data) {
    double s;
    if (strstr(type, "RMC")) {                         // SOG champ 7, valide si statut(2)='A'
        if (nf > 2 && f[2] && f[2][0] == 'A' && nmea_field_num(f, nf, 7, &s)) {
            data->sog = s; data->has_sog = true;
        }
    } else if (strstr(type, "VTG")) {                  // SOG champ 5, unité(6)='N'
        if (nmea_field_num(f, nf, 5, &s) &&
            (nf <= 6 || !f[6] || f[6][0] == '\0' || f[6][0] == 'N')) {
            data->sog = s; data->has_sog = true;
        }
    } else if (strstr(type, "VBW")) {                  // vitesse fond long. champ 4, statut(6)!='V'
        if (nmea_field_num(f, nf, 4, &s) &&
            (nf <= 6 || !f[6] || f[6][0] != 'V')) {
            data->sog = s; data->has_sog = true;
        }
    } else if (strstr(type, "RMA")) {                  // SOG champ 8, valide si statut(1)='A'
        if (nf > 1 && f[1] && f[1][0] == 'A' && nmea_field_num(f, nf, 8, &s)) {
            data->sog = s; data->has_sog = true;
        }
    } else if (strstr(type, "OSD")) {                  // vitesse champ 5, réf(6) sol P/B, unité(9)
        if (nmea_field_num(f, nf, 5, &s) &&
            nf > 6 && f[6] && (f[6][0] == 'P' || f[6][0] == 'B')) {
            if (nf > 9 && f[9] && f[9][0] == 'K') s /= 1.852;   // km/h -> nœuds
            data->sog = s; data->has_sog = true;
        }
    }
}

// Vitesse de vent ramenée en nœuds selon l'unité ('N'=nœuds, 'M'=m/s, 'K'=km/h).
static double wind_to_knots(double v, char unit) {
    if (unit == 'M') return v * 1.943844;   // m/s -> nœuds
    if (unit == 'K') return v / 1.852;       // km/h -> nœuds
    return v;                                 // 'N' (ou défaut) : déjà en nœuds
}

// TWA = direction vraie du vent − cap, ramené à [0,180]. Mis à jour dès qu'on a les deux.
static void nmea_update_twa(nmea_data_t *d) {
    if (d->has_mwv_true) return;   // vent eau (MWV,T) prioritaire : polaire = STW vs vent sur l'eau
    if (!d->has_twd || !d->has_heading) return;
    double rel = d->twd - d->heading;
    while (rel > 180.0) rel -= 360.0;
    while (rel < -180.0) rel += 360.0;
    d->twa = fabs(rel);
    d->has_twa = true;
}

bool parse_nmea_sentence(const char *sentence, nmea_data_t *data) {
    char line[PG_MAX_LINE];
    strncpy(line, sentence, PG_MAX_LINE - 1);
    line[PG_MAX_LINE - 1] = 0;

    char *p = line;
    while (*p && isspace(*p)) p++;
    if (*p == 0 || *p != '$') return false;

    size_t len = strlen(p);
    while (len > 0 && isspace(p[len-1])) p[--len] = 0;

    if (!verify_checksum(p)) return false;

    char *star = strchr(p, '*');
    if (star) *star = 0;

    // Découpage à champs préservés : indispensable car les positions sont fixes et
    // les champs souvent vides (ex. VHW sans cap vrai : $IIVHW,,T,25.0,M,5.9,N,..
    // -> le STW est toujours au champ 5). strtok fusionnait les vides et décalait tout.
    char *fields[24];
    int nf = nmea_split(p + 1, fields, 24);
    if (nf < 1) return false;
    const char *type = fields[0];

    // Émet un point dès que TWA+TWS+STW sont réunis — mais seulement sur les trames
    // vent/vitesse/cap (pas sur les trames SOG), pour ne pas surcompter.
    #define LIVE_COMPLETE() (data->has_twa && data->has_tws && data->has_bsp)

    if (strstr(type, "MWV")) {                 // vent vrai : angle(1) ref(2)=T vitesse(3) unité(4)
        double angle, speed;
        if (nf > 4 && nmea_field_num(fields, nf, 1, &angle) &&
            nmea_field_num(fields, nf, 3, &speed) &&
            fields[2][0] == 'T' && speed > 0.1) {
            double a = fmod(angle, 360.0);          // MWV : 0-360° depuis l'étrave, sens horaire
            if (a < 0) a += 360.0;
            data->twa = (a > 180.0) ? 360.0 - a : a; // -> TWA 0-180 (le bord bâbord est replié)
            data->tws = wind_to_knots(speed, fields[4][0]);
            data->has_twa = true;
            data->has_mwv_true = true;
            data->has_tws = true;
            if (LIVE_COMPLETE()) return true;
        }
    }
    else if (strstr(type, "MWD")) {            // dir vraie(1)=T, vitesse nœuds(5)/N ou m/s(7)/M
        double dir, speed;
        if (nf > 2 && nmea_field_num(fields, nf, 1, &dir) && fields[2][0] == 'T') {
            data->twd = dir;
            data->has_twd = true;
            if (!data->has_mwv_true) {             // vent eau (MWV,T) prioritaire : ne pas l'écraser
                if (nmea_field_num(fields, nf, 5, &speed) && speed > 0.1) {
                    data->tws = speed; data->has_tws = true;                      // nœuds
                } else if (nmea_field_num(fields, nf, 7, &speed) && speed > 0.1) {
                    data->tws = wind_to_knots(speed, 'M'); data->has_tws = true;  // m/s
                }
            }
            nmea_update_twa(data);             // TWA = TWD - cap (si cap connu)
            if (LIVE_COMPLETE()) return true;
        }
    }
    else if (strstr(type, "HDT") || strstr(type, "HDG")) {  // cap vrai au champ 1
        double hd;
        if (nmea_field_num(fields, nf, 1, &hd)) {
            data->heading = hd; data->has_heading = true;
            nmea_update_twa(data);
            if (LIVE_COMPLETE()) return true;
        }
    }
    else if (strstr(type, "VHW")) {            // STW nœuds au champ 5, unité 'N' au champ 6
        double speed;
        if (nf > 1 && fields[1][0] && nmea_field_num(fields, nf, 1, &speed)) {  // cap vrai (champ 1)
            data->heading = speed; data->has_heading = true; nmea_update_twa(data);
        }
        if (nf > 6 && fields[6][0] == 'N' && nmea_field_num(fields, nf, 5, &speed) && speed > 0.1) {
            data->bsp = speed;
            data->has_bsp = true;
            if (LIVE_COMPLETE()) return true;
        }
    }
    else {
        // Trames porteuses de SOG : mettent à jour l'état SOG sans « compléter » de point.
        parse_sog_sentence(type, fields, nf, data);
    }
    #undef LIVE_COMPLETE
    return false;
}

void add_data_point(polar_grid_t *grid, double twa, double tws, double bsp) {
    if (twa < 0 || twa > 180 || tws < 0 || tws > 70 || bsp < 0 || bsp > 50) return;

    int angle_bucket = round_to_bucket(twa, PG_ANGLE_STEP);
    int speed_bucket = round_to_bucket(tws, PG_SPEED_STEP);
    if (angle_bucket >= PG_MAX_ANGLES || speed_bucket >= PG_MAX_SPEEDS) return;

    data_point_t *point = malloc(sizeof(data_point_t));
    if (!point) return;

    point->bsp = bsp;
    point->next = grid->points[angle_bucket][speed_bucket];
    grid->points[angle_bucket][speed_bucket] = point;

    if (angle_bucket < grid->angle_min) grid->angle_min = angle_bucket;
    if (angle_bucket > grid->angle_max) grid->angle_max = angle_bucket;
    if (speed_bucket < grid->speed_min) grid->speed_min = speed_bucket;
    if (speed_bucket > grid->speed_max) grid->speed_max = speed_bucket;
    grid->point_count++;
}

// Agrège les BSP d'une cellule en retenant le percentile g_polar_percentile.
// Garde-fou anti-bruit : minimum 3 points, sinon 0. Le percentile élevé ignore
// naturellement le bas de distribution (faseyement, virements, ralentissements)
// sans qu'on ait à le rogner explicitement.
double aggregate_cell(data_point_t *head) {
    int count = 0;
    for (data_point_t *tmp = head; tmp; tmp = tmp->next) count++;
    if (count < 3) return 0.0;

    double *speeds = malloc(count * sizeof(double));
    if (!speeds) return 0.0;

    int i = 0;
    for (data_point_t *tmp = head; tmp; tmp = tmp->next) speeds[i++] = tmp->bsp;

    for (int a = 0; a < count - 1; a++)
        for (int b = a + 1; b < count; b++)
            if (speeds[a] > speeds[b]) {
                double tmp = speeds[a]; speeds[a] = speeds[b]; speeds[b] = tmp;
            }

    // Percentile par interpolation linéaire entre les deux échantillons encadrants
    double pos = (g_polar_percentile / 100.0) * (count - 1);
    int lo = (int)pos;
    double frac = pos - lo;
    double result = (lo + 1 < count) ? speeds[lo] * (1.0 - frac) + speeds[lo + 1] * frac
                                     : speeds[lo];

    free(speeds);
    return result;
}

double get_polar_value(polar_grid_t *grid, int angle, int speed) {
    if (angle < 0 || angle >= PG_MAX_ANGLES || speed < 0 || speed >= PG_MAX_SPEEDS) {
        return 0.0;
    }

    if (grid->cache_valid) {
        return grid->cached_polar[angle][speed];
    }

    return aggregate_cell(grid->points[angle][speed]);
}


/* ------------------------------------------------------------ NMEA 2000 --- */
/* Lecture native du NMEA 2000 au format texte YDRAW (Yacht Devices RAW) : une
 * trame CAN par ligne, « hh:mm:ss.ddd R 09F50303 00 1A 02 FF FF 00 FF FF ».
 * C'est ce que publient n2k-mux (TCP 2700), les passerelles Yacht Devices
 * (YDWG-02, YDEN-02…) et leurs enregistreurs. Les 4 PGN utiles tiennent chacun
 * dans une trame unique (pas de réassemblage fast-packet) :
 *   130306 Wind Data   : vitesse 0,01 m/s, angle 1e-4 rad, référence
 *                        (0 vrai/nord, 1 magnétique, 2 apparent, 3 vrai/bateau,
 *                        4 vrai/eau)
 *   128259 Speed       : STW 0,01 m/s
 *   129026 COG & SOG   : SOG 0,01 m/s
 *   127250 Heading     : cap 1e-4 rad, variation, référence (0 vrai, 1 magnétique)
 * Même contrat que parse_nmea_sentence : met à jour `data`, renvoie true quand une
 * trame vent/vitesse/cap complète un point TWA+TWS+STW. Même règle de vent : le
 * vent vrai rapporté à l'eau (réf. 4, = MWV,T) prime sur le vent fond (réf. 0 + cap,
 * = MWD) ; l'apparent est ignoré. */
#define N2K_MS_TO_KN 1.9438444924
#define N2K_RAD_TO_DEG (180.0 / M_PI)

static bool n2k_u16(const uint8_t *d, int off, int len, unsigned *out) {
    if (off + 2 > len) return false;
    unsigned v = (unsigned)d[off] | ((unsigned)d[off + 1] << 8);
    if (v >= 0xFFFD) return false;          // non disponible / hors plage / réservé
    *out = v;
    return true;
}

static double n2k_fold_angle(double deg) {  // angle à l'étrave 0-360 -> TWA 0-180
    double a = fmod(deg, 360.0);
    if (a < 0) a += 360.0;
    return (a > 180.0) ? 360.0 - a : a;
}

bool n2k_apply_frame(int pgn, const uint8_t *d, int len, nmea_data_t *data) {
    #define LIVE_COMPLETE() (data->has_twa && data->has_tws && data->has_bsp)
    unsigned v, a;
    switch (pgn) {
    case 130306: {                              // Wind Data
        if (len < 6 || !n2k_u16(d, 1, len, &v) || !n2k_u16(d, 3, len, &a)) return false;
        double tws = v * 0.01 * N2K_MS_TO_KN, ang = a * 1e-4 * N2K_RAD_TO_DEG;
        int ref = d[5] & 0x07;
        if (tws <= 0.1) return false;
        if (ref == 4) {                         // vrai rapporté à l'eau, angle / étrave
            data->twa = n2k_fold_angle(ang);
            data->tws = tws;
            data->has_twa = data->has_tws = data->has_mwv_true = true;
        } else if (data->has_mwv_true) {
            return false;                       // vent eau prioritaire : on ignore le vent fond
        } else if (ref == 3) {                  // vrai rapporté au fond, angle / étrave
            data->twa = n2k_fold_angle(ang);
            data->tws = tws;
            data->has_twa = data->has_tws = true;
        } else if (ref == 0) {                  // vrai rapporté au fond, direction / nord
            data->twd = ang; data->has_twd = true;
            data->tws = tws; data->has_tws = true;
            nmea_update_twa(data);
        } else {
            return false;                       // apparent (2) ou magnétique (1)
        }
        return LIVE_COMPLETE();
    }
    case 128259:                                // Speed, water referenced
        if (!n2k_u16(d, 1, len, &v) || v * 0.01 * N2K_MS_TO_KN <= 0.1) return false;
        data->bsp = v * 0.01 * N2K_MS_TO_KN;
        data->has_bsp = true;
        return LIVE_COMPLETE();
    case 129026:                                // COG & SOG, rapid update : SOG seul
        if (n2k_u16(d, 4, len, &v)) { data->sog = v * 0.01 * N2K_MS_TO_KN; data->has_sog = true; }
        return false;                           // comme les trames SOG 0183 : pas de point
    case 127250: {                              // Vessel Heading
        if (len < 8 || !n2k_u16(d, 1, len, &a)) return false;
        double hd = a * 1e-4 * N2K_RAD_TO_DEG;
        int ref = d[7] & 0x03;
        if (ref == 1) {                         // magnétique : vrai = mag + variation
            int16_t var = (int16_t)((unsigned)d[5] | ((unsigned)d[6] << 8));
            if (var == 0x7FFF) return false;    // variation inconnue : cap inutilisable
            hd += var * 1e-4 * N2K_RAD_TO_DEG;
        } else if (ref != 0) return false;
        data->heading = fmod(hd + 360.0, 360.0);
        data->has_heading = true;
        nmea_update_twa(data);
        return LIVE_COMPLETE();
    }
    default:
        return false;
    }
    #undef LIVE_COMPLETE
}

/* PGN d'un identifiant CAN 29 bits (J1939) : PDU1 (PF < 240) = adressé, l'octet PS
 * est la destination et ne fait pas partie du PGN ; PDU2 = diffusé, PS en fait partie. */
int n2k_pgn_from_id(uint32_t id) {
    unsigned pf = (id >> 16) & 0xFF, ps = (id >> 8) & 0xFF, dp = (id >> 24) & 0x3;
    return (int)((dp << 16) | (pf << 8) | (pf < 240 ? 0 : ps));
}

bool parse_ydraw_line(const char *line, nmea_data_t *data) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    // Horodatage hh:mm:ss.ddd
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) || p[2] != ':') return false;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p == ' ') p++;
    if (*p != 'R' && *p != 'T') return false;   // R = reçu du bus, T = émis sur le bus
    p++;
    char *end;
    unsigned long id = strtoul(p, &end, 16);
    if (end == p || id > 0x1FFFFFFFUL) return false;
    p = end;
    uint8_t d[8];
    int len = 0;
    while (len < 8) {
        while (*p == ' ') p++;
        if (!isxdigit((unsigned char)*p)) break;
        unsigned long b = strtoul(p, &end, 16);
        if (end - p != 2 || b > 0xFF) return false;
        d[len++] = (uint8_t)b;
        p = end;
    }
    if (len == 0) return false;
    return n2k_apply_frame(n2k_pgn_from_id((uint32_t)id), d, len, data);
}

/* ------------------------------------------------- passerelle Actisense --- */
/* Protocole série des passerelles Actisense NGT-1 / NGX-1 (mode Transfer), repris
 * de canboat/actisense-serial : trames DLE STX <commande> <longueur> <données>
 * <somme> DLE ETX, un DLE dans le contenu étant doublé ; la somme de tous les
 * octets de <commande> à <somme> vaut 0 (mod 256). Commande 0x93 = message N2K
 * reçu, déjà réassemblé par la passerelle : priorité, PGN (3 octets LE),
 * destination, source, horodatage (4), longueur, données. */
#define ACT_DLE 0x10
#define ACT_STX 0x02
#define ACT_ETX 0x03
#define ACT_N2K_RECEIVED 0x93
enum { ACT_START, ACT_MESSAGE, ACT_ESCAPE };

void actisense_rx_reset(actisense_rx_t *r) { r->n = 0; r->state = ACT_START; r->prev = ACT_START; }

// Commande « NGT » 0xA1 11 02 00 : vide la liste de PGN filtrés de la passerelle,
// qui émet alors tous les PGN (rétro-ingénierie canboat, renvoyée toutes les 20 s).
size_t actisense_startup_frame(uint8_t *out, size_t cap) {
    static const uint8_t body[] = { 0xA1, 3, 0x11, 0x02, 0x00 };
    uint8_t sum = 0;
    size_t o = 0;
    if (cap < 2 * (sizeof body + 1) + 4) return 0;
    out[o++] = ACT_DLE; out[o++] = ACT_STX;
    for (size_t i = 0; i <= sizeof body; i++) {
        uint8_t b = (i < sizeof body) ? body[i] : (uint8_t)(0x100 - sum);
        if (i < sizeof body) sum += b;
        if (b == ACT_DLE) out[o++] = ACT_DLE;
        out[o++] = b;
    }
    out[o++] = ACT_DLE; out[o++] = ACT_ETX;
    return o;
}

/* Un octet du flux série. true quand il termine un message N2K valide : *pgn,
 * *data (pointe dans r->buf, valable jusqu'au prochain appel) et *len. */
bool actisense_rx_byte(actisense_rx_t *r, uint8_t c, int *pgn, const uint8_t **data, int *len) {
    if (r->state == ACT_ESCAPE) {
        if (c == ACT_ETX) {
            r->state = ACT_START;
            size_t n = r->n;
            if (n < 3 || r->buf[0] != ACT_N2K_RECEIVED) return false;
            uint8_t sum = 0;
            for (size_t i = 0; i < n; i++) sum += r->buf[i];
            if (sum != 0) return false;                        // somme invalide
            size_t plen = r->buf[1];
            const uint8_t *m = r->buf + 2;
            if (plen < 11 || plen + 3 > n) return false;       // en-tête 0x93 incomplet
            size_t dlen = m[10];
            if (dlen > plen - 11) dlen = plen - 11;
            *pgn = (int)(m[1] | (m[2] << 8) | ((unsigned)m[3] << 16));
            *data = m + 11;
            *len = (int)dlen;
            return true;
        }
        if (c == ACT_STX) { r->n = 0; r->state = ACT_MESSAGE; return false; }
        if (c == ACT_DLE) {                                    // DLE doublé = octet 0x10
            if (r->prev == ACT_MESSAGE && r->n < sizeof r->buf) r->buf[r->n++] = c;
            r->state = r->prev;
            return false;
        }
        r->state = ACT_START;                                  // séquence invalide : resynchro
        return false;
    }
    if (c == ACT_DLE) { r->prev = r->state; r->state = ACT_ESCAPE; return false; }
    if (r->state == ACT_MESSAGE) {
        if (r->n < sizeof r->buf) r->buf[r->n++] = c;
        else r->state = ACT_START;                             // trop long : on abandonne
    }
    return false;
}

/* Une ligne d'un flux ou d'un fichier de navigation : NMEA 0183 ($…) ou N2K YDRAW. */
bool parse_nav_line(const char *line, nmea_data_t *data) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '$') return parse_nmea_sentence(p, data);
    if (isdigit((unsigned char)*p)) return parse_ydraw_line(p, data);
    return false;
}

void nmea_smoother_reset(nmea_smoother_t *s) {
    s->count = 0;
    s->head = 0;
    s->have_last = false;
}

// Ajoute un échantillon brut et renvoie la moyenne mobile (TWA/TWS/BSP). Vide la
// fenêtre sur un saut de TWA (virement/empannage) pour ne pas lisser au travers.
void nmea_smoother_push(nmea_smoother_t *s, double twa, double tws, double bsp,
                               double *out_twa, double *out_tws, double *out_bsp) {
    if (s->have_last && fabs(twa - s->last_twa) > NMEA_SMOOTH_TWA_BREAK)
        nmea_smoother_reset(s);
    s->last_twa = twa;
    s->have_last = true;

    s->twa[s->head] = twa;
    s->tws[s->head] = tws;
    s->bsp[s->head] = bsp;
    s->head = (s->head + 1) % NMEA_SMOOTH_BUFSZ;
    if (s->count < NMEA_SMOOTH_BUFSZ) s->count++;

    double sa = 0.0, sw = 0.0, sb = 0.0;
    for (int i = 0; i < s->count; i++) { sa += s->twa[i]; sw += s->tws[i]; sb += s->bsp[i]; }
    *out_twa = sa / s->count;
    *out_tws = sw / s->count;
    *out_bsp = sb / s->count;
}

int process_nmea_file(const char *filename, polar_grid_t *grid, ProgressContext *progress) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[PG_MAX_LINE];
    int line_count = 0, data_count = 0, filtered_count = 0;
    nmea_data_t current_data;
    memset(&current_data, 0, sizeof(nmea_data_t));
    nmea_smoother_t smoother;
    nmea_smoother_reset(&smoother);
    stw_sog_filter_t stwf;
    stw_sog_reset(&stwf);

    while (fgets(line, sizeof(line), f)) {
        line_count++;

        if (progress && line_count % 1000 == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Lecture du fichier NMEA...\n%d lignes lues, %d points collectés",
                     line_count, data_count);
            prog(progress, msg);
            if (*progress->cancel_flag) {
                fclose(f);
                return -1;
            }
        }

        if (parse_nav_line(line, &current_data)) {
            // Débruitage du loch : rejette un STW qui saute par rapport au SOG, AVANT
            // le lissage (pour qu'un pic ne pollue pas la moyenne mobile). Inactif
            // tant qu'aucune trame SOG n'a été vue.
            if (current_data.has_sog &&
                !stw_sog_accept(&stwf, current_data.bsp, current_data.sog)) {
                filtered_count++;
                continue;
            }

            double twa = current_data.twa, tws = current_data.tws, bsp = current_data.bsp;
            if (NMEA_SMOOTH_WINDOW > 1)
                nmea_smoother_push(&smoother, current_data.twa, current_data.tws,
                                   current_data.bsp, &twa, &tws, &bsp);

            if (g_polar_router) {
                // NMEA n'a pas de commentaires -> état voile/mer inconnu : le point ne
                // va que dans les polaires sans contrainte (critères « tout »).
                for (int k = 0; k < g_polar_router->n; k++)
                    if (polar_def_matches(&g_polar_router->defs[k], "", "", ""))
                        add_data_point(&g_polar_router->grids[k], twa, tws, bsp);
                data_count++;
                continue;
            }

            add_data_point(grid, twa, tws, bsp);
            data_count++;
        }
    }
    fclose(f);
    return data_count;
}

// Indique si la table VDR possède une colonne donnée (le schéma varie selon l'export qtVlm)
bool vdr_has_column(sqlite3 *db, const char *col) {
    sqlite3_stmt *st;
    bool found = false;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(VDR);", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && strcasecmp((const char *)name, col) == 0) { found = true; break; }
        }
        sqlite3_finalize(st);
    }
    return found;
}

// Normalise pour comparaison tolérante : translittère les accents en ASCII
// (é->e, ç->c…) puis passe en minuscules. À libérer avec g_free().
static char *kw_normalize(const char *s) {
    char *ascii = g_str_to_ascii(s, NULL);   // « Très forte » -> « Tres forte »
    char *down = g_ascii_strdown(ascii, -1);
    g_free(ascii);
    return down;
}

// Le commentaire contient-il le mot-clé ? Insensible à la casse ET aux accents
// (français), en sous-chaîne — ce qui gère aussi les commentaires accolés (« GVJ1 »
// contient « GV » et « J1 »).
bool comment_has_keyword(const char *comment, const char *keyword) {
    if (!comment || !keyword || !*keyword) return false;
    char *c = kw_normalize(comment);
    char *k = kw_normalize(keyword);
    bool found = (*k && strstr(c, k) != NULL);
    g_free(c);
    g_free(k);
    return found;
}

// Le commentaire mentionne-t-il une voile de l'inventaire (GV ou voile d'avant) ?
// Poser une voile signifie « reparti à la voile » -> termine le mode moteur.
static bool comment_is_sail(const BoatConfig *c, const char *comment) {
    if (!comment) return false;
    for (int i = 0; i < c->n_headsail; i++)
        if (comment_has_keyword(comment, c->headsail[i])) return true;
    for (int i = 0; i < c->n_mainsail; i++)
        if (comment_has_keyword(comment, c->mainsail[i])) return true;
    return false;
}

// État moteur déduit des commentaires (mots-clés du bateau) + RPM.
typedef enum { ENG_SAILING, ENG_CHARGING, ENG_MOTORING } engine_state_t;

int process_vdr_file(const char *filename, polar_grid_t *grid, ProgressContext *progress) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    // Le filtre moteur dépend d'un état (charge batteries hélice débrayée) qui se
    // propage dans le temps -> on traite RPM/COMMENT en C, pas en SQL. On récupère
    // donc toujours les colonnes aux mêmes indices (NULL si absentes du schéma) et
    // on ordonne par TIME pour pouvoir propager l'état « charge » d'un point au suivant.
    bool has_rpm = vdr_has_column(db, "RPM");
    bool has_comment = vdr_has_column(db, "COMMENT");
    bool has_time = vdr_has_column(db, "TIME");
    bool has_sog = vdr_has_column(db, "SOG");

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT TWA, TWS, STW, %s, %s, %s, %s FROM VDR "
             "WHERE TWA IS NOT NULL AND TWS IS NOT NULL AND STW IS NOT NULL AND STW > 0%s;",
             has_rpm ? "RPM" : "NULL",
             has_comment ? "COMMENT" : "NULL",
             has_sog ? "SOG" : "NULL",
             has_time ? "TIME" : "0",
             has_time ? " ORDER BY TIME" : "");

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    int data_count = 0, filtered_count = 0;
    engine_state_t engine = ENG_SAILING;  // forward-fill : voile / charge débrayée / moteur embrayé
    stw_sog_filter_t stwf;  // débruitage STW via SOG (offset courant suivi)
    stw_sog_reset(&stwf);
    long prev_time = 0;
    bool have_prev_time = false;
    // État courant pour le routage multi-polaires (forward-fill depuis les commentaires).
    char cur_main[BOAT_TERM_LEN] = "", cur_head[BOAT_TERM_LEN] = "", cur_sea[BOAT_TERM_LEN] = "";

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        double twa = fabs(sqlite3_column_double(stmt, 0));
        double tws = sqlite3_column_double(stmt, 1);
        double stw = sqlite3_column_double(stmt, 2);
        double rpm = (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
                     ? sqlite3_column_double(stmt, 3) : 0.0;
        const char *comment = (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
                              ? (const char *)sqlite3_column_text(stmt, 4) : NULL;
        bool has_sog_val = (sqlite3_column_type(stmt, 5) != SQLITE_NULL);
        double sog = has_sog_val ? sqlite3_column_double(stmt, 5) : 0.0;
        long t = (long)sqlite3_column_int64(stmt, 6);

        // Machine d'état moteur (forward-fill), pilotée par les mots-clés du bateau.
        // Charge = moteur débrayé (on garde) ; Moteur = embrayé (on exclut) ; une voile
        // posée = reparti à la voile (fin du mode moteur). Sans colonne RPM, le mode
        // Moteur ne se referme QUE sur un tag voile ; avec RPM, un RPM = 0 le referme aussi.
        if (comment) {
            if (comment_has_keyword(comment, g_boat_config.kw_charge))      engine = ENG_CHARGING;
            else if (comment_has_keyword(comment, g_boat_config.kw_moteur)) engine = ENG_MOTORING;
            else if (comment_is_sail(&g_boat_config, comment))             engine = ENG_SAILING;
            // État courant voile/mer (pour le routage) : on retient le terme reconnu.
            for (int i = 0; i < g_boat_config.n_mainsail; i++)
                if (comment_has_keyword(comment, g_boat_config.mainsail[i])) { g_strlcpy(cur_main, g_boat_config.mainsail[i], BOAT_TERM_LEN); break; }
            for (int i = 0; i < g_boat_config.n_headsail; i++)
                if (comment_has_keyword(comment, g_boat_config.headsail[i])) { g_strlcpy(cur_head, g_boat_config.headsail[i], BOAT_TERM_LEN); break; }
            for (int i = 0; i < g_boat_config.n_seastate; i++)
                if (comment_has_keyword(comment, g_boat_config.seastate[i])) { g_strlcpy(cur_sea, g_boat_config.seastate[i], BOAT_TERM_LEN); break; }
        }
        if (has_rpm && rpm <= 0.0) engine = ENG_SAILING;  // moteur coupé -> fin de session

        // Exclusion : moteur embrayé (tag Moteur), ou RPM > 0 sans annotation de charge.
        bool engine_on = has_rpm && rpm > 0.0;
        if (engine == ENG_MOTORING || (engine_on && engine != ENG_CHARGING)) {
            filtered_count++;
            continue;
        }

        if (twa < 0 || twa > 180 || tws < 0.1 || tws > 70 || stw < 0.1 || stw > 50) {
            continue;
        }

        // Débruitage du loch : rejette un STW qui s'écarte brutalement de l'offset
        // courant STW-SOG (pic de déjaugeage, roue à aube bloquée). Le courant lent
        // est absorbé par l'EMA. Ré-amorçage après un gros trou temporel. Inactif sans SOG.
        if (has_sog && has_sog_val) {
            if (have_prev_time && (t - prev_time) > STW_SOG_GAP_RESET) stw_sog_reset(&stwf);
            prev_time = t;
            have_prev_time = true;
            if (!stw_sog_accept(&stwf, stw, sog)) {
                filtered_count++;           // saut anormal -> on jette ce STW
                continue;
            }
        }

        if (progress && data_count % 1000 == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Lecture du fichier VDR...\n%d points collectés", data_count);
            prog(progress, msg);
            if (*progress->cancel_flag) {
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return -1;
            }
        }

        if (g_polar_router) {
            // Routage : ranger le point dans chaque polaire dont les critères matchent.
            for (int k = 0; k < g_polar_router->n; k++)
                if (polar_def_matches(&g_polar_router->defs[k], cur_main, cur_head, cur_sea))
                    add_data_point(&g_polar_router->grids[k], twa, tws, stw);
            data_count++;
            continue;
        }

        add_data_point(grid, twa, tws, stw);
        data_count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return data_count;
}

bool is_vdr_file(const char *filename) {
    size_t len = strlen(filename);
    if (len < 3) return false;
    const char *ext = filename + len - 3;
    return (strcasecmp(ext, ".db") == 0);
}

int process_file(const char *filename, polar_grid_t *grid, ProgressContext *progress) {
    if (is_vdr_file(filename)) {
        return process_vdr_file(filename, grid, progress);
    } else {
        return process_nmea_file(filename, grid, progress);
    }
}

bool load_existing_polar_for_update(const char *filename, polar_grid_t *grid, ProgressContext *progress) {
    FILE *f = fopen(filename, "r");
    if (!f) return false;

    if (progress) {
        prog(progress, "Chargement de la polaire existante...");
    }

    char line[PG_MAX_LINE];
    int speeds[PG_MAX_SPEEDS];
    int num_speeds = 0;
    bool header_read = false;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '!') continue;

        if (!header_read) {
            char *token = strtok(line, "\t\n");
            while ((token = strtok(NULL, "\t\n")) != NULL) {
                speeds[num_speeds++] = atoi(token);
            }
            header_read = true;
            continue;
        }

        char *token = strtok(line, "\t\n");
        if (!token) continue;
        int angle = atoi(token);

        for (int i = 0; i < num_speeds && (token = strtok(NULL, "\t\n")) != NULL; i++) {
            double bsp = atof(token);
            if (bsp > 0.1) {
                for (int j = 0; j < 5; j++) {
                    add_data_point(grid, angle, speeds[i], bsp);
                }
            }
        }
    }

    fclose(f);
    return true;
}

void compute_polar(polar_grid_t *grid, double result[PG_MAX_ANGLES][PG_MAX_SPEEDS], ProgressContext *progress) {
    for (int angle = 0; angle < PG_MAX_ANGLES; angle++) {
        if (progress && angle % 10 == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Calcul de la polaire...\n%d%% complété", (angle * 100) / PG_MAX_ANGLES);
            prog(progress, msg);
        }

        for (int speed = 0; speed < PG_MAX_SPEEDS; speed++) {
            result[angle][speed] = aggregate_cell(grid->points[angle][speed]);
            grid->cached_polar[angle][speed] = result[angle][speed];
        }
    }
    grid->cache_valid = true;
}

bool save_polar_to_file_pg(const char *filename, polar_grid_t *grid,
                            double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS], ProgressContext *progress) {
    FILE *f = fopen(filename, "w");
    if (!f) return false;

    if (progress) {
        prog(progress, "Sauvegarde de la polaire...");
    }

    int real_angle_min = 180, real_angle_max = 0;
    int real_speed_min = 100, real_speed_max = 0;

    for (int angle = 0; angle < PG_MAX_ANGLES; angle += PG_ANGLE_STEP) {
        for (int speed = 0; speed < PG_MAX_SPEEDS; speed += PG_SPEED_STEP) {
            if (polar[angle][speed] > 0) {
                if (angle < real_angle_min) real_angle_min = angle;
                if (angle > real_angle_max) real_angle_max = angle;
                if (speed < real_speed_min) real_speed_min = speed;
                if (speed > real_speed_max) real_speed_max = speed;
            }
        }
    }

    fprintf(f, "TWA\\TWS");
    for (int speed = real_speed_min; speed <= real_speed_max; speed += PG_SPEED_STEP)
        fprintf(f, "\t%d", speed);
    fprintf(f, "\n");

    for (int angle = real_angle_min; angle <= real_angle_max; angle += PG_ANGLE_STEP) {
        fprintf(f, "%d", angle);
        for (int speed = real_speed_min; speed <= real_speed_max; speed += PG_SPEED_STEP) {
            fprintf(f, "\t%.2f", polar[angle][speed]);
        }
        fprintf(f, "\n");
    }
    fclose(f);

    return true;
}

//==============================================================================
// FIN FONCTIONS POLAR_GENERATOR
//==============================================================================

// Charger la polaire existante depuis les données en mémoire pour la mise à jour
