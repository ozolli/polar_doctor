/*
 * polar_generator.c
 * Generateur de diagrammes polaires pour voiliers
 * Compilation: gcc -o polar_generator polar_generator.c -lm -lsqlite3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdbool.h>
#include <sqlite3.h>

#define MAX_LINE 512
#define MAX_SPEEDS 100
#define MAX_ANGLES 181
#define ANGLE_STEP 5
#define SPEED_STEP 2

typedef struct {
    double tws, twa, bsp;
    bool has_tws, has_twa, has_bsp;
} nmea_data_t;

typedef struct data_point {
    double bsp;
    struct data_point *next;
} data_point_t;

typedef struct {
    data_point_t *points[MAX_ANGLES][MAX_SPEEDS];
    double cached_polar[MAX_ANGLES][MAX_SPEEDS];
    bool cache_valid;
    int angle_min, angle_max, speed_min, speed_max, point_count;
} polar_grid_t;

void init_polar_grid(polar_grid_t *grid) {
    memset(grid, 0, sizeof(polar_grid_t));
    grid->angle_min = 180;
    grid->speed_min = 100;
    grid->cache_valid = false;
}

void free_polar_grid(polar_grid_t *grid) {
    for (int i = 0; i < MAX_ANGLES; i++) {
        for (int j = 0; j < MAX_SPEEDS; j++) {
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
    if (sentence[0] != 36) return false;
    const char *star = strchr(sentence, 42);
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

bool parse_nmea_sentence(const char *sentence, nmea_data_t *data) {
    char line[MAX_LINE], parse_buf[MAX_LINE];
    strncpy(line, sentence, MAX_LINE - 1);
    line[MAX_LINE - 1] = 0;
    
    char *p = line;
    while (*p && isspace(*p)) p++;
    if (*p == 0 || *p != 36) return false;
    
    size_t len = strlen(p);
    while (len > 0 && isspace(p[len-1])) p[--len] = 0;
    
    if (!verify_checksum(p)) return false;
    
    strncpy(parse_buf, p, MAX_LINE - 1);
    parse_buf[MAX_LINE - 1] = 0;
    
    char *star = strchr(parse_buf, 42);
    if (star) *star = 0;
    
    char *saveptr = NULL;
    char *msg_type = strtok_r(parse_buf + 1, ",", &saveptr);
    if (!msg_type) return false;
    
    if (strstr(msg_type, "MWV")) {
        char *angle_str = strtok_r(NULL, ",", &saveptr);
        char *reference = strtok_r(NULL, ",", &saveptr);
        char *speed_str = strtok_r(NULL, ",", &saveptr);
        char *unit = strtok_r(NULL, ",", &saveptr);
        
        if (angle_str && reference && speed_str && unit) {
            double angle = atof(angle_str);
            double speed = atof(speed_str);
            
            if (reference[0] == 84 && speed > 0.1 && unit[0] == 78) {
                data->twa = fabs(angle);
                data->tws = speed;
                data->has_twa = true;
                data->has_tws = true;
                if (data->has_bsp) return true;
            }
        }
    }
    else if (strstr(msg_type, "VHW")) {
        char *f1 = strtok_r(NULL, ",", &saveptr);
        char *f2 = strtok_r(NULL, ",", &saveptr);
        char *speed_knots = strtok_r(NULL, ",", &saveptr);
        char *unit_knots = strtok_r(NULL, ",", &saveptr);
        
        if (speed_knots && unit_knots && unit_knots[0] == 78) {
            double speed = atof(speed_knots);
            if (speed > 0.1) {
                data->bsp = speed;
                data->has_bsp = true;
                if (data->has_twa && data->has_tws) return true;
            }
        }
    }
    return false;
}

void add_data_point(polar_grid_t *grid, double twa, double tws, double bsp) {
    if (twa < 0 || twa > 180 || tws < 0 || tws > 50 || bsp < 0 || bsp > 20) return;
    
    int angle_bucket = round_to_bucket(twa, ANGLE_STEP);
    int speed_bucket = round_to_bucket(tws, SPEED_STEP);
    if (angle_bucket >= MAX_ANGLES || speed_bucket >= MAX_SPEEDS) return;
    
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

double get_polar_value(polar_grid_t *grid, int angle, int speed) {
    if (angle < 0 || angle >= MAX_ANGLES || speed < 0 || speed >= MAX_SPEEDS) {
        return 0.0;
    }
    
    if (grid->cache_valid) {
        return grid->cached_polar[angle][speed];
    }
    
    data_point_t *p = grid->points[angle][speed];
    if (!p) return 0.0;
    
    int count = 0;
    for (data_point_t *tmp = p; tmp; tmp = tmp->next) count++;
    if (count < 3) return 0.0;
    
    double *speeds = malloc(count * sizeof(double));
    if (!speeds) return 0.0;
    
    int i = 0;
    for (data_point_t *tmp = p; tmp; tmp = tmp->next) speeds[i++] = tmp->bsp;
    
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (speeds[i] > speeds[j]) {
                double tmp = speeds[i]; speeds[i] = speeds[j]; speeds[j] = tmp;
            }
    
    double result;
    if (count >= 5) {
        int trim = count / 5;
        if (trim < 1) trim = 1;
        double sum = 0;
        int valid_count = 0;
        for (int i = trim; i < count - trim; i++) { sum += speeds[i]; valid_count++; }
        result = valid_count > 0 ? sum / valid_count : 0.0;
    } else {
        double sum = 0;
        for (int i = 0; i < count; i++) sum += speeds[i];
        result = sum / count;
    }
    
    free(speeds);
    return result;
}

int process_nmea_file(const char *filename, polar_grid_t *grid, bool verbose, bool update_mode) {
    FILE *f = fopen(filename, "r");
    if (!f) { 
        fprintf(stderr, "Erreur: impossible d'ouvrir %s\n", filename); 
        return -1; 
    }
    
    printf("Lecture du fichier NMEA: %s\n", filename);
    char line[MAX_LINE];
    int line_count = 0, data_count = 0, filtered_count = 0;
    nmea_data_t current_data;
    memset(&current_data, 0, sizeof(nmea_data_t));
    
    while (fgets(line, sizeof(line), f)) {
        line_count++;
        if (parse_nmea_sentence(line, &current_data)) {
            if (update_mode) {
                int angle_bucket = round_to_bucket(current_data.twa, ANGLE_STEP);
                int speed_bucket = round_to_bucket(current_data.tws, SPEED_STEP);
                double existing = get_polar_value(grid, angle_bucket, speed_bucket);
                
                if (existing > 0.0 && current_data.bsp < existing * 0.95) {
                    filtered_count++;
                    continue;
                }
            }
            
            add_data_point(grid, current_data.twa, current_data.tws, current_data.bsp);
            data_count++;
            if (verbose && data_count % 100 == 0)
                printf("  %d points collectes...\n", data_count);
        }
    }
    fclose(f);
    
    printf("  Lignes lues: %d\n", line_count);
    printf("  Points valides: %d\n", data_count);
    if (update_mode && filtered_count > 0) {
        printf("  Points filtres (performances inferieures): %d\n", filtered_count);
    }
    return data_count;
}

int process_vdr_file(const char *filename, polar_grid_t *grid, bool verbose, bool update_mode) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;
    
    rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Erreur: impossible d'ouvrir la base SQLite %s: %s\n", 
                filename, sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    
    printf("Lecture du fichier VDR: %s\n", filename);
    
    const char *sql = "SELECT TWA, TWS, STW FROM VDR WHERE TWA IS NOT NULL AND TWS IS NOT NULL AND STW IS NOT NULL AND STW > 0;";
    
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Erreur SQL: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    
    int data_count = 0, filtered_count = 0;
    
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        double twa = fabs(sqlite3_column_double(stmt, 0));
        double tws = sqlite3_column_double(stmt, 1);
        double stw = sqlite3_column_double(stmt, 2);
        
        if (twa < 0 || twa > 180 || tws < 0.1 || tws > 50 || stw < 0.1 || stw > 20) {
            continue;
        }
        
        if (update_mode) {
            int angle_bucket = round_to_bucket(twa, ANGLE_STEP);
            int speed_bucket = round_to_bucket(tws, SPEED_STEP);
            double existing = get_polar_value(grid, angle_bucket, speed_bucket);
            
            if (existing > 0.0 && stw < existing * 0.95) {
                filtered_count++;
                continue;
            }
        }
        
        add_data_point(grid, twa, tws, stw);
        data_count++;
        
        if (verbose && data_count % 100 == 0)
            printf("  %d points collectes...\n", data_count);
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    
    printf("  Points valides: %d\n", data_count);
    if (update_mode && filtered_count > 0) {
        printf("  Points filtres (performances inferieures): %d\n", filtered_count);
    }
    
    return data_count;
}

bool is_vdr_file(const char *filename) {
    size_t len = strlen(filename);
    if (len < 3) return false;
    const char *ext = filename + len - 3;
    return (strcasecmp(ext, ".db") == 0);
}

int process_file(const char *filename, polar_grid_t *grid, bool verbose, bool update_mode) {
    if (is_vdr_file(filename)) {
        return process_vdr_file(filename, grid, verbose, update_mode);
    } else {
        return process_nmea_file(filename, grid, verbose, update_mode);
    }
}

bool load_existing_polar(const char *filename, polar_grid_t *grid) {
    FILE *f = fopen(filename, "r");
    if (!f) return false;
    
    printf("Chargement de la polaire existante: %s\n", filename);
    
    char line[MAX_LINE];
    int speeds[MAX_SPEEDS];
    int num_speeds = 0;
    bool header_read = false;
    int loaded_cells = 0;
    
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
                // Ajouter 5 points identiques pour avoir suffisamment de données
                for (int j = 0; j < 5; j++) {
                    add_data_point(grid, angle, speeds[i], bsp);
                }
                loaded_cells++;
            }
        }
    }
    
    fclose(f);
    printf("  Cellules chargees: %d\n", loaded_cells);
    printf("  Points generes: %d\n", grid->point_count);
    return true;
}

void compute_polar(polar_grid_t *grid, double result[MAX_ANGLES][MAX_SPEEDS]) {
    for (int angle = 0; angle < MAX_ANGLES; angle++) {
        for (int speed = 0; speed < MAX_SPEEDS; speed++) {
            result[angle][speed] = 0.0;
            data_point_t *p = grid->points[angle][speed];
            if (!p) continue;
            
            int count = 0;
            for (data_point_t *tmp = p; tmp; tmp = tmp->next) count++;
            if (count < 3) continue;
            
            if (count >= 5) {
                double *speeds = malloc(count * sizeof(double));
                int i = 0;
                for (data_point_t *tmp = p; tmp; tmp = tmp->next) speeds[i++] = tmp->bsp;
                
                for (int i = 0; i < count - 1; i++)
                    for (int j = i + 1; j < count; j++)
                        if (speeds[i] > speeds[j]) {
                            double tmp = speeds[i]; speeds[i] = speeds[j]; speeds[j] = tmp;
                        }
                
                int trim = count / 5;
                if (trim < 1) trim = 1;
                double sum = 0;
                int valid_count = 0;
                for (int i = trim; i < count - trim; i++) { sum += speeds[i]; valid_count++; }
                result[angle][speed] = valid_count > 0 ? sum / valid_count : 0.0;
                free(speeds);
            } else {
                double sum = 0;
                for (data_point_t *tmp = p; tmp; tmp = tmp->next) sum += tmp->bsp;
                result[angle][speed] = sum / count;
            }
            
            grid->cached_polar[angle][speed] = result[angle][speed];
        }
    }
    grid->cache_valid = true;
}

bool save_polar_to_file(const char *filename,
                        polar_grid_t *grid, double polar[MAX_ANGLES][MAX_SPEEDS]) {
    FILE *f = fopen(filename, "w");
    if (!f) { perror("Erreur ouverture fichier"); return false; }
    
    // Déterminer les vraies limites en incluant toutes les données
    int real_angle_min = 180, real_angle_max = 0;
    int real_speed_min = 100, real_speed_max = 0;
    
    for (int angle = 0; angle < MAX_ANGLES; angle += ANGLE_STEP) {
        for (int speed = 0; speed < MAX_SPEEDS; speed += SPEED_STEP) {
            if (polar[angle][speed] > 0) {
                if (angle < real_angle_min) real_angle_min = angle;
                if (angle > real_angle_max) real_angle_max = angle;
                if (speed < real_speed_min) real_speed_min = speed;
                if (speed > real_speed_max) real_speed_max = speed;
            }
        }
    }
    
    fprintf(f, "TWA\\TWS");
    for (int speed = real_speed_min; speed <= real_speed_max; speed += SPEED_STEP)
        fprintf(f, "\t%d", speed);
    fprintf(f, "\n");
    
    int data_count = 0;
    for (int angle = real_angle_min; angle <= real_angle_max; angle += ANGLE_STEP) {
        fprintf(f, "%d", angle);
        for (int speed = real_speed_min; speed <= real_speed_max; speed += SPEED_STEP) {
            double bsp = polar[angle][speed];
            if (bsp > 0) data_count++;
            fprintf(f, "\t%.2f", bsp);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    
    printf("Polaire sauvegardee dans %s\n", filename);
    printf("  - Angles: %d a %d degres (pas de %d degres)\n", 
           real_angle_min, real_angle_max, ANGLE_STEP);
    printf("  - Vitesses de vent: %d a %d noeuds (pas de %d noeuds)\n",
           real_speed_min, real_speed_max, SPEED_STEP);
    printf("  - Cellules remplies: %d\n", data_count);
    
    return true;
}

void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS] fichier [...]\n\n", progname);
    printf("Genere ou met a jour un diagramme polaire a partir de fichiers NMEA0183 ou VDR\n\n");
    printf("Options:\n");
    printf("  -o FICHIER  Fichier de sortie (defaut: polar.pol)\n");
    printf("  -i FICHIER  Charge une polaire existante a mettre a jour\n");
    printf("  -v          Mode verbeux\n");
    printf("  -h          Affiche cette aide\n\n");
    printf("Formats supportes:\n");
    printf("  - Fichiers NMEA0183 (.nmea, .txt, .log): messages MWV + VHW\n");
    printf("  - Fichiers VDR SQLite (.db): generes par qtVlm\n\n");
    printf("Exemples:\n");
    printf("  Creation initiale:\n");
    printf("    %s -o polaire.pol sortie1.nmea\n", progname);
    printf("    %s -o polaire.pol trace.db\n\n", progname);
    printf("  Mise a jour avec nouvelles donnees:\n");
    printf("    %s -i polaire.pol -o polaire.pol sortie2.nmea trace.db\n\n", progname);
    printf("Note: En mode mise a jour, seules les performances egales ou\n");
    printf("      superieures a l'existant sont conservees.\n");
}

int main(int argc, char *argv[]) {
    const char *output_file = "polar.pol", *input_polar = NULL;
    bool verbose = false;
    char *input_files[100];
    int num_files = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_file = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) input_polar = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) verbose = true;
        else if (strcmp(argv[i], "-h") == 0) { print_usage(argv[0]); return 0; }
        else if (argv[i][0] != 45) input_files[num_files++] = argv[i];
    }
    
    if (num_files == 0 && !input_polar) { 
        fprintf(stderr, "Erreur: aucun fichier specifie\n\n"); 
        print_usage(argv[0]); 
        return 1; 
    }
    
    polar_grid_t grid;
    init_polar_grid(&grid);
    
    bool update_mode = false;
    if (input_polar) {
        if (load_existing_polar(input_polar, &grid)) {
            printf("\nCalcul de la polaire de reference...\n");
            compute_polar(&grid, grid.cached_polar);
            grid.cache_valid = true;
            update_mode = true;
        } else {
            printf("Impossible de charger %s, creation d'une nouvelle polaire\n", input_polar);
        }
    }
    
    for (int i = 0; i < num_files; i++)
        if (process_file(input_files[i], &grid, verbose, update_mode) < 0) { 
            free_polar_grid(&grid); 
            return 1; 
        }
    
    if (grid.point_count == 0) { 
        fprintf(stderr, "Aucune donnee collectee\n"); 
        free_polar_grid(&grid); 
        return 1; 
    }
    
    printf("\nCalcul de la polaire...\n");
    double polar[MAX_ANGLES][MAX_SPEEDS];
    compute_polar(&grid, polar);
    
    printf("Sauvegarde de la polaire...\n");
    if (!save_polar_to_file(output_file, &grid, polar)) { 
        free_polar_grid(&grid); 
        return 1; 
    }
    
    free_polar_grid(&grid);
    printf("\nTermine!\n");
    return 0;
}
