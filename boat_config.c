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
    fclose(f);
    return true;
}
