#include "polar_doctor.h"

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

    if (strstr(type, "MWV")) {                 // angle(1) ref(2) vitesse(3) unité(4)
        double angle, speed;
        if (nf > 4 && nmea_field_num(fields, nf, 1, &angle) &&
            nmea_field_num(fields, nf, 3, &speed) &&
            fields[2][0] == 'T' && speed > 0.1 && fields[4][0] == 'N') {
            data->twa = fabs(angle);
            data->tws = speed;
            data->has_twa = true;
            data->has_tws = true;
            if (data->has_bsp) return true;
        }
    }
    else if (strstr(type, "VHW")) {            // STW nœuds au champ 5, unité 'N' au champ 6
        double speed;
        if (nf > 6 && fields[6][0] == 'N' && nmea_field_num(fields, nf, 5, &speed) &&
            speed > 0.1) {
            data->bsp = speed;
            data->has_bsp = true;
            if (data->has_twa && data->has_tws) return true;
        }
    }
    else {
        // Trames porteuses de SOG : mettent à jour l'état SOG sans « compléter » de
        // point (le point reste déclenché par la paire MWV+VHW).
        parse_sog_sentence(type, fields, nf, data);
    }
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

// Moyenne mobile circulaire des derniers points NMEA complets (voir NMEA_SMOOTH_WINDOW).
typedef struct {
    double twa[NMEA_SMOOTH_BUFSZ];
    double tws[NMEA_SMOOTH_BUFSZ];
    double bsp[NMEA_SMOOTH_BUFSZ];
    int count;        // échantillons valides dans la fenêtre (<= BUFSZ)
    int head;         // index d'écriture circulaire
    double last_twa;  // pour détecter une manœuvre
    bool have_last;
} nmea_smoother_t;

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

int process_nmea_file(const char *filename, polar_grid_t *grid, bool update_mode, ProgressContext *progress) {
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
            gtk_label_set_text(GTK_LABEL(progress->label), msg);
            while (gtk_events_pending()) gtk_main_iteration();
            if (*progress->cancel_flag) {
                fclose(f);
                return -1;
            }
        }

        if (parse_nmea_sentence(line, &current_data)) {
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

            if (update_mode) {
                int angle_bucket = round_to_bucket(twa, PG_ANGLE_STEP);
                int speed_bucket = round_to_bucket(tws, PG_SPEED_STEP);
                double existing = get_polar_value(grid, angle_bucket, speed_bucket);

                if (existing > 0.0 && bsp < existing * 0.95) {
                    filtered_count++;
                    continue;
                }
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

// Recherche insensible à la casse d'un mot-clé dans un commentaire VDR libre.
bool comment_has_keyword(const char *comment, const char *keyword) {
    if (!comment || !keyword || !*keyword) return false;
    size_t klen = strlen(keyword);
    for (const char *p = comment; *p; p++)
        if (strncasecmp(p, keyword, klen) == 0) return true;
    return false;
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

int process_vdr_file(const char *filename, polar_grid_t *grid, bool update_mode, ProgressContext *progress) {
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
            gtk_label_set_text(GTK_LABEL(progress->label), msg);
            while (gtk_events_pending()) gtk_main_iteration();
            if (*progress->cancel_flag) {
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return -1;
            }
        }

        if (update_mode) {
            int angle_bucket = round_to_bucket(twa, PG_ANGLE_STEP);
            int speed_bucket = round_to_bucket(tws, PG_SPEED_STEP);
            double existing = get_polar_value(grid, angle_bucket, speed_bucket);

            if (existing > 0.0 && stw < existing * 0.95) {
                filtered_count++;
                continue;
            }
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

int process_file(const char *filename, polar_grid_t *grid, bool update_mode, ProgressContext *progress) {
    if (is_vdr_file(filename)) {
        return process_vdr_file(filename, grid, update_mode, progress);
    } else {
        return process_nmea_file(filename, grid, update_mode, progress);
    }
}

bool load_existing_polar_for_update(const char *filename, polar_grid_t *grid, ProgressContext *progress) {
    FILE *f = fopen(filename, "r");
    if (!f) return false;

    if (progress) {
        gtk_label_set_text(GTK_LABEL(progress->label), "Chargement de la polaire existante...");
        while (gtk_events_pending()) gtk_main_iteration();
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
            gtk_label_set_text(GTK_LABEL(progress->label), msg);
            while (gtk_events_pending()) gtk_main_iteration();
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
        gtk_label_set_text(GTK_LABEL(progress->label), "Sauvegarde de la polaire...");
        while (gtk_events_pending()) gtk_main_iteration();
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
