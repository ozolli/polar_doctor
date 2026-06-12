#include "polar_doctor.h"

void boat_config_init(BoatConfig *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->kw_moteur, BOAT_TERM_LEN, "Moteur");
    snprintf(c->kw_charge, BOAT_TERM_LEN, "Charge");
}

// Rogne les espaces de début/fin, en place.
char *boat_str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

void boat_list_add(char list[][BOAT_TERM_LEN], int *n, const char *term) {
    if (*n >= BOAT_MAX_ITEMS || !term || !*term) return;
    snprintf(list[*n], BOAT_TERM_LEN, "%s", term);
    (*n)++;
}

// Découpe une CSV en liste (trim, ignore vide et « * » qui signifie « tout »).
static void boat_csv_to_list(const char *csv, char list[][BOAT_TERM_LEN], int *n) {
    *n = 0;
    char buf[256];
    g_strlcpy(buf, csv ? csv : "", sizeof(buf));
    char *sp = NULL;
    for (char *tok = strtok_r(buf, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        char *t = boat_str_trim(tok);
        if (*t && strcmp(t, "*") != 0) boat_list_add(list, n, t);
    }
}

// Joint une liste en CSV, ou « * » si vide.
static const char *boat_list_to_csv(const char list[][BOAT_TERM_LEN], int n, char *buf, size_t sz) {
    if (n <= 0) { g_strlcpy(buf, "*", sz); return buf; }
    buf[0] = 0;
    for (int i = 0; i < n; i++) {
        if (i) g_strlcat(buf, ", ", sz);
        g_strlcat(buf, list[i], sz);
    }
    return buf;
}

static bool boat_in_list(const char list[][BOAT_TERM_LEN], int n, const char *val) {
    if (n == 0) return true;            // dimension non contrainte = tout
    if (!val || !*val) return false;    // contrainte mais valeur courante inconnue
    for (int i = 0; i < n; i++)
        if (g_ascii_strcasecmp(list[i], val) == 0) return true;
    return false;
}

bool polar_def_matches(const PolarDef *pd, const char *cur_main,
                       const char *cur_head, const char *cur_sea) {
    return boat_in_list(pd->mains, pd->n_mains, cur_main)
        && boat_in_list(pd->heads, pd->n_heads, cur_head)
        && boat_in_list(pd->seas,  pd->n_seas,  cur_sea);
}

// Charge un boat.cfg (format INI). true si le fichier a pu être lu.
bool boat_config_load(BoatConfig *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    boat_config_init(c);
    char line[256], section[32] = "";
    while (fgets(line, sizeof(line), f)) {
        char *p = boat_str_trim(line);
        if (*p == 0 || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) { *end = 0; snprintf(section, sizeof(section), "%s", p + 1); }
            continue;
        }
        if (strcmp(section, "boat") == 0 || strcmp(section, "engine") == 0) {
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = boat_str_trim(p), *v = boat_str_trim(eq + 1);
            if (strcmp(section, "boat") == 0 && strcmp(k, "name") == 0)
                snprintf(c->name, sizeof(c->name), "%s", v);
            else if (strcmp(k, "moteur") == 0) snprintf(c->kw_moteur, BOAT_TERM_LEN, "%s", v);
            else if (strcmp(k, "charge") == 0) snprintf(c->kw_charge, BOAT_TERM_LEN, "%s", v);
        } else if (strcmp(section, "mainsail") == 0) {
            boat_list_add(c->mainsail, &c->n_mainsail, p);
        } else if (strcmp(section, "headsails") == 0) {
            boat_list_add(c->headsail, &c->n_headsail, p);
        } else if (strcmp(section, "seastates") == 0) {
            boat_list_add(c->seastate, &c->n_seastate, p);
        } else if (strcmp(section, "polars") == 0) {
            char *eq = strchr(p, '=');
            if (!eq || c->n_polars >= BOAT_MAX_POLARS) continue;
            *eq = 0;
            PolarDef *pd = &c->polars[c->n_polars];
            memset(pd, 0, sizeof(*pd));
            g_strlcpy(pd->name, boat_str_trim(p), sizeof(pd->name));
            char *sp = NULL;                       // segments « clé: csv » séparés par ';'
            for (char *seg = strtok_r(eq + 1, ";", &sp); seg; seg = strtok_r(NULL, ";", &sp)) {
                char *colon = strchr(seg, ':');
                if (!colon) continue;
                *colon = 0;
                char *key = boat_str_trim(seg);
                char *csv = colon + 1;
                if (strcmp(key, "mains") == 0)      boat_csv_to_list(csv, pd->mains, &pd->n_mains);
                else if (strcmp(key, "heads") == 0) boat_csv_to_list(csv, pd->heads, &pd->n_heads);
                else if (strcmp(key, "seas") == 0)  boat_csv_to_list(csv, pd->seas,  &pd->n_seas);
            }
            c->n_polars++;
        }
    }
    fclose(f);
    return true;
}

bool boat_config_save(const BoatConfig *c, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "[boat]\nname = %s\n\n", c->name);
    fprintf(f, "[mainsail]            ; états de grand-voile (un terme par ligne)\n");
    for (int i = 0; i < c->n_mainsail; i++) fprintf(f, "%s\n", c->mainsail[i]);
    fprintf(f, "\n[headsails]           ; voiles d'avant\n");
    for (int i = 0; i < c->n_headsail; i++) fprintf(f, "%s\n", c->headsail[i]);
    fprintf(f, "\n[seastates]           ; états de mer\n");
    for (int i = 0; i < c->n_seastate; i++) fprintf(f, "%s\n", c->seastate[i]);
    fprintf(f, "\n[engine]\nmoteur = %s\ncharge = %s\n", c->kw_moteur, c->kw_charge);
    if (c->n_polars > 0) {
        char b1[256], b2[256], b3[256];
        fprintf(f, "\n[polars]              ; nom = mains: … ; heads: … ; seas: …  (* = tout)\n");
        for (int i = 0; i < c->n_polars; i++) {
            const PolarDef *pd = &c->polars[i];
            fprintf(f, "%s = mains: %s ; heads: %s ; seas: %s\n", pd->name,
                    boat_list_to_csv(pd->mains, pd->n_mains, b1, sizeof(b1)),
                    boat_list_to_csv(pd->heads, pd->n_heads, b2, sizeof(b2)),
                    boat_list_to_csv(pd->seas,  pd->n_seas,  b3, sizeof(b3)));
        }
    }
    fclose(f);
    return true;
}

// Trouve le fichier de config d'un dossier-bateau : boat.cfg, sinon <nom>.cfg,
// sinon le premier *.cfg. Renvoie true et remplit out (chemin complet).
bool boat_find_config(const char *folder, char *out, size_t outsz) {
    char *p = g_build_filename(folder, "boat.cfg", NULL);          // 1) boat.cfg
    if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) {
        g_strlcpy(out, p, outsz); g_free(p); return true;
    }
    g_free(p);

    char *base = g_path_get_basename(folder);                       // 2) <nom>.cfg
    char *named = g_strdup_printf("%s.cfg", base);
    p = g_build_filename(folder, named, NULL);
    bool ok = g_file_test(p, G_FILE_TEST_IS_REGULAR);
    if (ok) g_strlcpy(out, p, outsz);
    g_free(p); g_free(named); g_free(base);
    if (ok) return true;

    GDir *d = g_dir_open(folder, 0, NULL);                          // 3) premier *.cfg
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (g_str_has_suffix(name, ".cfg")) {
                char *q = g_build_filename(folder, name, NULL);
                g_strlcpy(out, q, outsz); g_free(q);
                g_dir_close(d); return true;
            }
        }
        g_dir_close(d);
    }
    return false;
}

// Chemin du fichier des bateaux récents (~/.config/polar_doctor/recent_boats),
// crée le dossier au besoin. À libérer avec g_free().
static char *boat_recent_path(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "polar_doctor", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *p = g_build_filename(dir, "recent_boats", NULL);
    g_free(dir);
    return p;
}

int boat_recent_load(char list[][BOAT_PATH_LEN], int max) {
    char *path = boat_recent_path();
    FILE *f = fopen(path, "r");
    g_free(path);
    if (!f) return 0;
    int n = 0;
    char line[BOAT_PATH_LEN];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *t = boat_str_trim(line);
        if (*t) { g_strlcpy(list[n], t, BOAT_PATH_LEN); n++; }
    }
    fclose(f);
    return n;
}

// Place boat_folder en tête de la liste des récents (dédoublonné, plafonné).
void boat_recent_add(const char *boat_folder) {
    char list[BOAT_RECENT_MAX + 1][BOAT_PATH_LEN];
    int n = boat_recent_load(list, BOAT_RECENT_MAX + 1);
    char *path = boat_recent_path();
    FILE *f = fopen(path, "w");
    g_free(path);
    if (!f) return;
    fprintf(f, "%s\n", boat_folder);
    int written = 1;
    for (int i = 0; i < n && written < BOAT_RECENT_MAX; i++) {
        if (strcmp(list[i], boat_folder) != 0) { fprintf(f, "%s\n", list[i]); written++; }
    }
    fclose(f);
}
