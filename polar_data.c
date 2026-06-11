#include "polar_doctor.h"

void load_polar_from_memory(PolarData *data, polar_grid_t *grid) {
    // Convertir les données en mémoire vers la grille polar_generator
    for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;

        int angle = data->twa_values[angle_idx];
        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            int speed = data->tws_values[speed_idx];
            double bsp = data->polar_data[angle_idx][speed_idx];

            if (bsp > 0.1) {
                // Ajouter plusieurs points pour avoir suffisamment de données
                for (int j = 0; j < 5; j++) {
                    add_data_point(grid, angle, speed, bsp);
                }
            }
        }
    }
}

// Interpolation bilinéaire pour obtenir une valeur BSP depuis la grille
double interpolate_bsp(double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS], double twa, double tws) {
    // Trouver les buckets encadrants pour TWA
    int twa_low = ((int)(twa / PG_ANGLE_STEP)) * PG_ANGLE_STEP;
    int twa_high = twa_low + PG_ANGLE_STEP;

    // Trouver les buckets encadrants pour TWS
    int tws_low = ((int)(tws / PG_SPEED_STEP)) * PG_SPEED_STEP;
    int tws_high = tws_low + PG_SPEED_STEP;

    // Vérifier les limites
    if (twa_low < 0) twa_low = 0;
    if (twa_high >= PG_MAX_ANGLES) twa_high = PG_MAX_ANGLES - 1;
    if (tws_low < 0) tws_low = 0;
    if (tws_high >= PG_MAX_SPEEDS) tws_high = PG_MAX_SPEEDS - 1;

    // Récupérer les 4 valeurs des coins
    double v00 = polar[twa_low][tws_low];   // Coin bas-gauche
    double v10 = polar[twa_high][tws_low];  // Coin haut-gauche
    double v01 = polar[twa_low][tws_high];  // Coin bas-droite
    double v11 = polar[twa_high][tws_high]; // Coin haut-droite

    // Si on est exactement sur un bucket, retourner directement
    if (twa_low == twa_high && tws_low == tws_high) {
        return v00;
    }

    // Calculer les coefficients d'interpolation (entre 0 et 1)
    double t_twa = 0.0, t_tws = 0.0;

    if (twa_high > twa_low) {
        t_twa = (twa - twa_low) / (double)(twa_high - twa_low);
    }

    if (tws_high > tws_low) {
        t_tws = (tws - tws_low) / (double)(tws_high - tws_low);
    }

    // Interpolation bilinéaire
    // D'abord interpoler selon TWS (horizontal)
    double v0 = v00 * (1.0 - t_tws) + v01 * t_tws;  // Interpolation à TWA_low
    double v1 = v10 * (1.0 - t_tws) + v11 * t_tws;  // Interpolation à TWA_high

    // Puis interpoler selon TWA (vertical)
    double result = v0 * (1.0 - t_twa) + v1 * t_twa;

    return result;
}

// Mettre à jour une polaire existante depuis la grille (conserve la structure TWS/TWA)
void update_polar_from_grid(PolarData *data, double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS]) {
    // Parcourir les angles et vitesses existants dans la polaire
    for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;

        int twa = data->twa_values[angle_idx];

        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            int tws = data->tws_values[speed_idx];

            // Utiliser l'interpolation bilinéaire pour obtenir la valeur BSP
            double new_bsp = interpolate_bsp(polar, (double)twa, (double)tws);

            if (new_bsp > 0.0) {
                double old_bsp = data->polar_data[angle_idx][speed_idx];

                // Ne mettre à jour que si la nouvelle valeur est >= 95% de l'ancienne
                // (ou si l'ancienne valeur est 0)
                if (old_bsp == 0.0 || new_bsp >= old_bsp * 0.95) {
                    data->polar_data[angle_idx][speed_idx] = new_bsp;
                    snprintf(data->polar_data_str[angle_idx][speed_idx], 16, "%.2f", new_bsp);
                }
            }
        }
    }

    data->modified = TRUE;
}

// Charger une polaire depuis la grille polar_generator directement en mémoire
void load_polar_from_grid(PolarData *data, polar_grid_t *grid, double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS]) {
    // Réinitialiser les données
    init_polar_data(data);

    // Déterminer les vraies limites
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

    // Remplir les valeurs TWS — toujours commencer par la colonne TWS 0
    // (en-tête historique requis par les fichiers .pol/.csv pour l'import)
    gboolean has_zero_col = (real_speed_min == 0);
    data->num_speeds = 0;
    if (!has_zero_col) {
        data->tws_values[data->num_speeds++] = 0;
    }
    for (int speed = real_speed_min; speed <= real_speed_max; speed += PG_SPEED_STEP) {
        data->tws_values[data->num_speeds++] = speed;
    }

    // Remplir les valeurs TWA et les données
    data->num_angles = 0;
    for (int angle = real_angle_min; angle <= real_angle_max; angle += PG_ANGLE_STEP) {
        int angle_idx = data->num_angles;
        data->twa_values[angle_idx] = angle;
        data->twa_present[angle_idx] = 1;

        int speed_idx = 0;
        if (!has_zero_col) {
            // Colonne TWS 0 : valeurs nulles
            data->polar_data[angle_idx][speed_idx] = 0.0;
            strcpy(data->polar_data_str[angle_idx][speed_idx], "0.00");
            speed_idx++;
        }
        for (int speed = real_speed_min; speed <= real_speed_max; speed += PG_SPEED_STEP) {
            double bsp = polar[angle][speed];
            data->polar_data[angle_idx][speed_idx] = bsp;
            snprintf(data->polar_data_str[angle_idx][speed_idx], 16, "%.2f", bsp);
            speed_idx++;
        }
        data->num_angles++;
    }

    data->modified = TRUE;
    data->filename[0] = '\0';  // Pas encore sauvegardé
}

// Convertir l'index de la combo box vers l'index réel dans tws_values[]
// (nécessaire car TWS 0 est exclu des combo boxes)
int combo_index_to_tws_index(PolarData *data, int combo_idx) {
    int count = 0;
    for (int i = 0; i < data->num_speeds; i++) {
        if (data->tws_values[i] == 0) continue;  // Sauter TWS 0
        if (count == combo_idx) return i;
        count++;
    }
    return data->num_speeds - 1;  // Par défaut, retourner le dernier
}

// Index par défaut pour le combo TWS "to" : ~7e vitesse ou la dernière disponible.
// Basé sur le nombre réel d'entrées du combo (TWS 0 exclu) pour éviter un index hors limites.
int tws_default_to_index(PolarData *data) {
    int combo_count = 0;
    for (int i = 0; i < data->num_speeds; i++) {
        if (data->tws_values[i] != 0) combo_count++;
    }
    if (combo_count <= 0) return 0;
    int last_idx = combo_count - 1;
    return last_idx > 6 ? 6 : last_idx;
}

// Initialiser les données de la polaire
void init_polar_data(PolarData *data) {
    memset(data->polar_data, 0, sizeof(data->polar_data));
    memset(data->polar_data_str, 0, sizeof(data->polar_data_str));
    memset(data->twa_present, 0, sizeof(data->twa_present));
    memset(data->twa_values, 0, sizeof(data->twa_values));

    // Valeurs par défaut pour pouvoir créer l'interface
    int speeds[] = {0, 4, 6, 8, 10, 12, 14, 16, 20, 25, 30, 35, 40, 45, 50, 60};
    data->num_speeds = 16;
    for (int i = 0; i < data->num_speeds; i++) {
        data->tws_values[i] = speeds[i];
    }
    data->num_angles = 0;

    strcpy(data->filename, "");
    data->modified = FALSE;
}

// Charger un fichier .pol
gboolean load_polar_file(const char *filename, PolarData *data) {
    FILE *f = fopen(filename, "r");
    if (!f) return FALSE;

    // Réinitialiser
    memset(data->polar_data, 0, sizeof(data->polar_data));
    memset(data->polar_data_str, 0, sizeof(data->polar_data_str));
    memset(data->twa_present, 0, sizeof(data->twa_present));
    memset(data->twa_values, 0, sizeof(data->twa_values));

    char line[2048];
    int speeds[MAX_SPEEDS];
    int num_speeds = 0;
    int angle_idx = 0;
    gboolean header_read = FALSE;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '!') continue;
        if (strlen(line) < 2) continue;

        // Déterminer le séparateur (tabulation ou point-virgule)
        char *sep = strchr(line, ';') ? ";\n" : "\t\n";

        if (!header_read) {
            // Lire l'en-tête avec les vitesses de vent (toutes les colonnes)
            char *token = strtok(line, sep);
            while ((token = strtok(NULL, sep)) != NULL && num_speeds < MAX_SPEEDS) {
                speeds[num_speeds++] = atoi(token);
            }
            data->num_speeds = num_speeds;
            for (int i = 0; i < num_speeds; i++) {
                data->tws_values[i] = speeds[i];
            }
            header_read = TRUE;
            continue;
        }

        // Lire les données
        char *token = strtok(line, sep);
        if (!token) continue;
        int angle = atoi(token);

        if (angle_idx >= MAX_ANGLES) continue;

        // Stocker l'angle TWA réel
        data->twa_values[angle_idx] = angle;
        data->twa_present[angle_idx] = 1;

        for (int i = 0; i < num_speeds; i++) {
            token = strtok(NULL, sep);
            if (token) {
                double value = atof(token);
                data->polar_data[angle_idx][i] = value;
                // Stocker la valeur avec format fixe 2 décimales
                snprintf(data->polar_data_str[angle_idx][i], 16, "%.2f", value);
            }
        }
        angle_idx++;
    }

    data->num_angles = angle_idx;

    fclose(f);
    strncpy(data->filename, filename, sizeof(data->filename) - 1);
    data->filename[sizeof(data->filename) - 1] = '\0';
    data->modified = FALSE;
    return TRUE;
}

// Sauvegarder un fichier .pol
gboolean save_polar_file(const char *filename, PolarData *data) {
    // Ajouter l'extension .pol si non présente
    char final_filename[512];
    strncpy(final_filename, filename, sizeof(final_filename) - 5);
    final_filename[sizeof(final_filename) - 5] = '\0';

    // Vérifier si le fichier se termine déjà par .pol
    size_t len = strlen(final_filename);
    if (len < 4 || strcasecmp(final_filename + len - 4, ".pol") != 0) {
        strcat(final_filename, ".pol");
    }

    FILE *f = fopen(final_filename, "w");
    if (!f) return FALSE;

    // Déterminer le séparateur à utiliser (même que le fichier d'origine)
    const char *sep = ";";  // Par défaut point-virgule comme CM50.pol

    // En-tête
    fprintf(f, "TWA\\TWS");
    for (int i = 0; i < data->num_speeds; i++) {
        fprintf(f, "%s%d", sep, data->tws_values[i]);
    }
    fprintf(f, "\n");

    // Données - seulement les lignes TWA présentes dans le fichier d'origine
    for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;  // Ignorer les lignes absentes

        int angle = data->twa_values[angle_idx];
        fprintf(f, "%d", angle);
        for (int i = 0; i < data->num_speeds; i++) {
            // Utiliser la valeur texte originale (pas de conversion/arrondi)
            fprintf(f, "%s%s", sep, data->polar_data_str[angle_idx][i]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    strncpy(data->filename, final_filename, sizeof(data->filename) - 1);
    data->filename[sizeof(data->filename) - 1] = '\0';
    data->modified = FALSE;
    return TRUE;
}

// Callback pour dessiner le diagramme polaire
// Interpolation bilinéaire de la BSP dans une PolarData (grille TWA x TWS irrégulière).
// twa_values et tws_values sont supposés triés croissants ; clampe hors limites.
double interpolate_polar_bsp(PolarData *data, double twa, double tws) {
    if (data->num_angles < 1 || data->num_speeds < 1) return 0.0;

    // Rangées TWA présentes encadrant twa
    int r_lo = -1, r_hi = -1;
    for (int i = 0; i < data->num_angles; i++) {
        if (!data->twa_present[i]) continue;
        if (data->twa_values[i] <= twa) r_lo = i;
        if (data->twa_values[i] >= twa && r_hi < 0) r_hi = i;
    }
    if (r_lo < 0) r_lo = r_hi;
    if (r_hi < 0) r_hi = r_lo;
    if (r_lo < 0) return 0.0;

    // Colonnes TWS encadrant tws
    int c_lo = -1, c_hi = -1;
    for (int j = 0; j < data->num_speeds; j++) {
        if (data->tws_values[j] <= tws) c_lo = j;
        if (data->tws_values[j] >= tws && c_hi < 0) c_hi = j;
    }
    if (c_lo < 0) c_lo = c_hi;
    if (c_hi < 0) c_hi = c_lo;
    if (c_lo < 0) return 0.0;

    double f_twa = (data->twa_values[r_hi] != data->twa_values[r_lo])
        ? (twa - data->twa_values[r_lo]) / (double)(data->twa_values[r_hi] - data->twa_values[r_lo]) : 0.0;
    double f_tws = (data->tws_values[c_hi] != data->tws_values[c_lo])
        ? (tws - data->tws_values[c_lo]) / (double)(data->tws_values[c_hi] - data->tws_values[c_lo]) : 0.0;

    double v_ll = data->polar_data[r_lo][c_lo], v_lr = data->polar_data[r_lo][c_hi];
    double v_hl = data->polar_data[r_hi][c_lo], v_hr = data->polar_data[r_hi][c_hi];
    double v_lo = v_ll * (1.0 - f_tws) + v_lr * f_tws;  // à r_lo
    double v_hi = v_hl * (1.0 - f_tws) + v_hr * f_tws;  // à r_hi
    return v_lo * (1.0 - f_twa) + v_hi * f_twa;
}

// BSP maximale sur la courbe d'une TWS donnée + le TWA où elle culmine (parmi les rangées présentes)
double dynamic_curve_max(PolarData *data, double tws, double *out_twa) {
    double best = 0.0, best_twa = 0.0;
    for (int i = 0; i < data->num_angles; i++) {
        if (!data->twa_present[i]) continue;
        double bsp = interpolate_polar_bsp(data, data->twa_values[i], tws);
        if (bsp > best) { best = bsp; best_twa = data->twa_values[i]; }
    }
    if (out_twa) *out_twa = best_twa;
    return best;
}

// BSP maximale sur toute la polaire + la TWS et le TWA correspondants
double polar_absolute_max(PolarData *data, double *out_tws, double *out_twa) {
    double best = 0.0, best_tws = 0.0, best_twa = 0.0;
    for (int i = 0; i < data->num_angles; i++) {
        if (!data->twa_present[i]) continue;
        for (int j = 0; j < data->num_speeds; j++) {
            if (data->polar_data[i][j] > best) {
                best = data->polar_data[i][j];
                best_tws = data->tws_values[j];
                best_twa = data->twa_values[i];
            }
        }
    }
    if (out_tws) *out_tws = best_tws;
    if (out_twa) *out_twa = best_twa;
    return best;
}

// Couleur distincte par TWS (indexée sur la colonne), partagée entre courbes et légende.
void tws_palette_color(int idx, double *r, double *g, double *b) {
    static const double pal[][3] = {
        {0.20, 0.40, 1.00},  // bleu
        {0.00, 0.80, 0.00},  // vert
        {0.90, 0.85, 0.00},  // jaune
        {1.00, 0.00, 1.00},  // magenta
        {1.00, 0.50, 0.40},  // saumon
        {0.65, 0.65, 0.65},  // gris
        {1.00, 0.55, 0.00},  // orange
        {0.40, 0.60, 1.00},  // bleu clair
        {1.00, 0.40, 0.70},  // rose
        {0.60, 0.60, 0.20},  // olive
        {0.40, 1.00, 0.50},  // vert clair
        {0.00, 0.80, 0.80},  // cyan
    };
    int k = ((idx % 12) + 12) % 12;
    *r = pal[k][0]; *g = pal[k][1]; *b = pal[k][2];
}

// Angles VMG optimaux d'une courbe (près et portant) pour une TWS donnée, par balayage fin
// interpolé. En deçà de a_up (près trop serré) et au-delà de a_dn (portant trop bas) : VMG dégradé.
void vmg_optimal_angles(PolarData *data, double tws, double *a_up, double *a_dn) {
    double best_up = -1e9, best_dn = -1e9;
    *a_up = 0.0; *a_dn = 180.0;
    for (double a = 0.0; a <= 180.0; a += 0.1) {
        double bsp = interpolate_polar_bsp(data, a, tws);
        if (bsp < 0.01) continue;
        double vmg = bsp * cos(a * M_PI / 180.0);  // >0 au près, <0 au portant
        if (a < 90.0 && vmg > best_up) { best_up = vmg; *a_up = a; }
        if (a > 90.0 && -vmg > best_dn) { best_dn = -vmg; *a_dn = a; }
    }
}

// Trace une courbe polaire lissée (Catmull-Rom), dans la couleur (ur,ug,ub) sur la plage
// VMG utile [a_up, a_dn] et rouge en dehors (zones de VMG dégradé).
