/*
 * polar_doctor.c
 * Interface graphique GTK pour l'édition de diagrammes polaires
 * Compilation: gcc -o polar_doctor polar_doctor.c -lm -lsqlite3 `pkg-config --cflags --libs gtk+-3.0`
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <stdbool.h>
#include <sqlite3.h>

// Configuration spécifique Windows pour les dialogues de fichiers
#ifdef _WIN32
#include <windows.h>
#define GTK_FILE_CHOOSER_NATIVE_DISABLED 1
#endif

#define MAX_ANGLES 37    // 0° à 180° par pas de 5°
#define MAX_SPEEDS 16    // 4 à 60 kn par pas de variable
#define ANGLE_STEP 5
#define MIN_TWS 4
#define MAX_TWS 60

// Constantes pour polar_generator
#define PG_MAX_LINE 512
#define PG_MAX_SPEEDS 100
#define PG_MAX_ANGLES 181
#define PG_ANGLE_STEP 5
#define PG_SPEED_STEP 2

// Structures pour polar_generator
typedef struct {
    double tws, twa, bsp;
    bool has_tws, has_twa, has_bsp;
} nmea_data_t;

typedef struct data_point {
    double bsp;
    struct data_point *next;
} data_point_t;

typedef struct {
    data_point_t *points[PG_MAX_ANGLES][PG_MAX_SPEEDS];
    double cached_polar[PG_MAX_ANGLES][PG_MAX_SPEEDS];
    bool cache_valid;
    int angle_min, angle_max, speed_min, speed_max, point_count;
} polar_grid_t;

typedef struct {
    GtkWidget *dialog;
    GtkWidget *label;
    gboolean *cancel_flag;
} ProgressContext;

typedef struct {
    double polar_data[MAX_ANGLES][MAX_SPEEDS];
    char polar_data_str[MAX_ANGLES][MAX_SPEEDS][16];  // Valeurs originales en texte
    int tws_values[MAX_SPEEDS];
    int num_speeds;
    int twa_values[MAX_ANGLES];   // Valeurs TWA réelles (pas forcément multiples de 5)
    int twa_present[MAX_ANGLES];  // 1 si cette ligne TWA existe dans le fichier
    int num_angles;               // Nombre d'angles présents
    char filename[256];
    gboolean modified;
} PolarData;

typedef enum {
    LANG_FR,
    LANG_EN
} Language;

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *grid_view;
    GtkWidget *polar_view;
    GtkWidget *status_bar;
    GtkWidget *grid_table;
    GtkWidget *tws_from_combo;
    GtkWidget *tws_to_combo;
    GtkWidget *vmg_container;  // Container pour le tableau VMG
    GtkWidget *lang_button;    // Bouton de langue
    // Boutons de la toolbar
    GtkWidget *btn_open;
    GtkWidget *btn_save;
    GtkWidget *btn_create;
    GtkWidget *btn_update;
    GtkWidget *btn_print;
    GtkWidget *btn_add_twa;
    GtkWidget *btn_add_tws;
    GtkWidget *btn_delete;
    GtkWidget *btn_help;
    // Labels pour traduction
    GtkWidget *tws_from_label;
    GtkWidget *tws_to_label;
    PolarData *polar_data;
    gboolean delete_mode;  // Mode suppression actif
    guint blink_timer_id;  // ID du timer pour le clignotement
    gboolean blink_state;  // État du clignotement
    Language language;     // Langue actuelle
} AppWidgets;

// Structure pour passer les coordonnées de cellule au callback
typedef struct {
    AppWidgets *app;
    int angle_idx;
    int speed_idx;
} CellData;

// Structure pour passer l'info d'en-tête au callback
typedef struct {
    AppWidgets *app;
    int index;
    gboolean is_twa;  // TRUE pour TWA (ligne), FALSE pour TWS (colonne)
} HeaderData;

// Traductions
typedef struct {
    const char *fr;
    const char *en;
} Translation;

#define TR(app, str_fr, str_en) ((app)->language == LANG_FR ? (str_fr) : (str_en))

// Forward declarations
void init_polar_data(PolarData *data);
void rebuild_data_tab(AppWidgets *app);
void update_data_tab(AppWidgets *app);
void rebuild_vmg_table(AppWidgets *app);
void on_tws_changed(GtkWidget *widget, gpointer user_data);
void on_cell_changed(GtkEntry *entry, gpointer user_data);
gboolean prompt_save_changes(AppWidgets *app);
void on_print_clicked(GtkWidget *widget, gpointer user_data);
void print_begin(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data);
void print_page(GtkPrintOperation *operation, GtkPrintContext *context, gint page_nr, gpointer user_data);
void on_create_clicked(GtkWidget *widget, gpointer user_data);
void on_update_clicked(GtkWidget *widget, gpointer user_data);
void on_add_twa_clicked(GtkWidget *widget, gpointer user_data);
void on_add_tws_clicked(GtkWidget *widget, gpointer user_data);
void on_delete_clicked(GtkWidget *widget, gpointer user_data);
gboolean on_header_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_help_clicked(GtkWidget *widget, gpointer user_data);
void on_lang_clicked(GtkWidget *widget, gpointer user_data);

// ============================================================================
// Dialogues de fichiers natifs Windows (évite les crashes GTK)
// ============================================================================

#ifdef _WIN32

// Convertit UTF-8 vers wide char pour Windows
static wchar_t* utf8_to_wchar(const char *utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len == 0) return NULL;
    wchar_t *wstr = (wchar_t*)malloc(len * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, len);
    return wstr;
}

// Convertit wide char vers UTF-8
static char* wchar_to_utf8(const wchar_t *wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len == 0) return NULL;
    char *utf8 = (char*)malloc(len);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, len, NULL, NULL);
    return utf8;
}

// Dialogue "Enregistrer sous" natif Windows
static char* win32_save_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                const char *filter_pattern, const char *default_name) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};

    // Convertir le titre
    wchar_t *wtitle = utf8_to_wchar(title);

    // Préparer le nom par défaut
    if (default_name) {
        wchar_t *wdefault = utf8_to_wchar(default_name);
        if (wdefault) {
            wcsncpy(szFile, wdefault, MAX_PATH - 1);
            free(wdefault);
        }
    }

    // Construire le filtre (format: "Description\0*.ext\0\0")
    wchar_t filter[256];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 256, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;  // Pas de parent pour éviter les problèmes GTK
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    char *result = NULL;
    if (GetSaveFileNameW(&ofn) == TRUE) {
        result = wchar_to_utf8(szFile);
    }

    if (wtitle) free(wtitle);
    return result;
}

// Dialogue "Ouvrir" natif Windows
static char* win32_open_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                const char *filter_pattern) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};

    wchar_t *wtitle = utf8_to_wchar(title);

    // Construire le filtre
    wchar_t filter[256];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 256, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    char *result = NULL;
    if (GetOpenFileNameW(&ofn) == TRUE) {
        result = wchar_to_utf8(szFile);
    }

    if (wtitle) free(wtitle);
    return result;
}

// Dialogue "Ouvrir" multi-sélection natif Windows
static GSList* win32_open_multi_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                        const char *filter_pattern) {
    OPENFILENAMEW ofn;
    wchar_t szFile[8192] = {0};  // Buffer plus grand pour multi-sélection

    wchar_t *wtitle = utf8_to_wchar(title);

    // Construire le filtre
    wchar_t filter[512];
    if (filter_name && filter_pattern) {
        wchar_t *wfilter_name = utf8_to_wchar(filter_name);
        wchar_t *wfilter_pattern = utf8_to_wchar(filter_pattern);
        swprintf(filter, 512, L"%s%c%s%c%c", wfilter_name, L'\0', wfilter_pattern, L'\0', L'\0');
        free(wfilter_name);
        free(wfilter_pattern);
    } else {
        wcscpy(filter, L"All Files (*.*)\0*.*\0\0");
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    GSList *file_list = NULL;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        wchar_t *ptr = szFile;
        wchar_t dir[MAX_PATH];

        // Premier élément : répertoire ou fichier unique
        wcscpy(dir, ptr);
        ptr += wcslen(ptr) + 1;

        // Si ptr pointe sur une chaîne vide, c'est un fichier unique
        if (*ptr == L'\0') {
            char *filepath = wchar_to_utf8(dir);
            if (filepath) {
                file_list = g_slist_append(file_list, filepath);
            }
        } else {
            // Multi-sélection : dir contient le répertoire, puis les noms de fichiers
            while (*ptr) {
                wchar_t fullpath[MAX_PATH];
                swprintf(fullpath, MAX_PATH, L"%s\\%s", dir, ptr);
                char *filepath = wchar_to_utf8(fullpath);
                if (filepath) {
                    file_list = g_slist_append(file_list, filepath);
                }
                ptr += wcslen(ptr) + 1;
            }
        }
    }

    if (wtitle) free(wtitle);
    return file_list;
}

#endif  // _WIN32
void update_interface_language(AppWidgets *app);

//==============================================================================
// FONCTIONS POLAR_GENERATOR INTEGREES
//==============================================================================

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

bool parse_nmea_sentence(const char *sentence, nmea_data_t *data) {
    char line[PG_MAX_LINE], parse_buf[PG_MAX_LINE];
    strncpy(line, sentence, PG_MAX_LINE - 1);
    line[PG_MAX_LINE - 1] = 0;

    char *p = line;
    while (*p && isspace(*p)) p++;
    if (*p == 0 || *p != '$') return false;

    size_t len = strlen(p);
    while (len > 0 && isspace(p[len-1])) p[--len] = 0;

    if (!verify_checksum(p)) return false;

    strncpy(parse_buf, p, PG_MAX_LINE - 1);
    parse_buf[PG_MAX_LINE - 1] = 0;

    char *star = strchr(parse_buf, '*');
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

            if (reference[0] == 'T' && speed > 0.1 && unit[0] == 'N') {
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

        if (speed_knots && unit_knots && unit_knots[0] == 'N') {
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

double get_polar_value(polar_grid_t *grid, int angle, int speed) {
    if (angle < 0 || angle >= PG_MAX_ANGLES || speed < 0 || speed >= PG_MAX_SPEEDS) {
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

int process_nmea_file(const char *filename, polar_grid_t *grid, bool update_mode, ProgressContext *progress) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[PG_MAX_LINE];
    int line_count = 0, data_count = 0, filtered_count = 0;
    nmea_data_t current_data;
    memset(&current_data, 0, sizeof(nmea_data_t));

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
            if (update_mode) {
                int angle_bucket = round_to_bucket(current_data.twa, PG_ANGLE_STEP);
                int speed_bucket = round_to_bucket(current_data.tws, PG_SPEED_STEP);
                double existing = get_polar_value(grid, angle_bucket, speed_bucket);

                if (existing > 0.0 && current_data.bsp < existing * 0.95) {
                    filtered_count++;
                    continue;
                }
            }

            add_data_point(grid, current_data.twa, current_data.tws, current_data.bsp);
            data_count++;
        }
    }
    fclose(f);
    return data_count;
}

int process_vdr_file(const char *filename, polar_grid_t *grid, bool update_mode, ProgressContext *progress) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_open(filename, &db);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    const char *sql = "SELECT TWA, TWS, STW FROM VDR WHERE TWA IS NOT NULL AND TWS IS NOT NULL AND STW IS NOT NULL AND STW > 0;";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
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

    // Remplir les valeurs TWS
    data->num_speeds = 0;
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
static gboolean draw_polar_diagram(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    PolarData *data = app->polar_data;

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    // Positionner le centre sur le côté gauche au milieu
    int margin = 80;
    int center_x = margin;
    int center_y = height / 2;
    int radius = height / 2 - margin;

    // Fond gris clair
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_paint(cr);

    // Demi-cercle blanc de fond (0° à 180°, côté tribord)
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, center_x, center_y - radius); // Départ à 0°
    cairo_arc(cr, center_x, center_y, radius, -M_PI / 2, M_PI / 2); // Arc de -90° à +90°
    cairo_line_to(cr, center_x, center_y); // Retour au centre
    cairo_close_path(cr);
    cairo_fill(cr);

    // Récupérer les indices TWS sélectionnés
    int combo_from = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_from_combo));
    int combo_to = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_to_combo));

    // Convertir les indices de combo box vers les indices réels
    int tws_from_idx = (combo_from >= 0) ? combo_index_to_tws_index(data, combo_from) : 0;
    int tws_to_idx = (combo_to >= 0) ? combo_index_to_tws_index(data, combo_to) : data->num_speeds - 1;

    if (tws_from_idx > tws_to_idx) {
        int tmp = tws_from_idx;
        tws_from_idx = tws_to_idx;
        tws_to_idx = tmp;
    }

    // Trouver la vitesse maximale du bateau (BSP) pour les TWS affichés uniquement
    double max_bsp = 0.0;
    for (int i = 0; i < MAX_ANGLES; i++) {
        for (int j = tws_from_idx; j <= tws_to_idx && j < data->num_speeds; j++) {
            if (data->polar_data[i][j] > max_bsp) {
                max_bsp = data->polar_data[i][j];
            }
        }
    }

    // Arrondir à l'entier supérieur
    int max_scale = (int)ceil(max_bsp);
    if (max_scale < 1) max_scale = 10;

    // Grille circulaire (arcs de 0° à 180°)
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_set_line_width(cr, 1.0);

    // Dessiner les cercles de 2 en 2 (ou de 1 en 1 si échelle petite)
    int step = max_scale > 10 ? 2 : 1;
    for (int i = step; i <= max_scale; i += step) {
        double r = radius * i / (double)max_scale;
        cairo_new_path(cr);
        cairo_arc(cr, center_x, center_y, r, -M_PI / 2, M_PI / 2);
        cairo_stroke(cr);

        // Légende BSP (vitesse bateau) à gauche du centre, alignées verticalement
        cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10);

        char label[16];
        snprintf(label, sizeof(label), "%d", i);

        // Positionner le label sur une ligne verticale fixe, à la hauteur du cercle
        cairo_text_extents_t extents;
        cairo_text_extents(cr, label, &extents);
        // Position: ligne verticale fixe légèrement à droite du centre, hauteur = center_y - r
        cairo_move_to(cr, center_x + 1, center_y - 6 - r + extents.height / 2);
        cairo_show_text(cr, label);
    }

    // Lignes radiales (tous les 15°)
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    for (int angle = 0; angle <= 180; angle += 15) {
        double rad = angle * M_PI / 180.0;
        cairo_new_path(cr);
        cairo_move_to(cr, center_x, center_y);
        cairo_line_to(cr, center_x + radius * sin(rad), center_y - radius * cos(rad));
        cairo_stroke(cr);

        // Légende TWA (angles) tous les 15°
        if (angle % 15 == 0) {
            cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11);

            char angle_label[16];
            snprintf(angle_label, sizeof(angle_label), "%d°", angle);

            double label_r = radius + 20;
            double label_x = center_x + label_r * sin(rad);
            double label_y = center_y - label_r * cos(rad);

            cairo_move_to(cr, label_x - 12, label_y + 5);
            cairo_show_text(cr, angle_label);
        }
    }

    // Dessiner les courbes polaires pour chaque TWS
    double colors[][3] = {
        {0.0, 0.0, 1.0},   // bleu
        {1.0, 0.0, 0.0},   // rouge
        {0.0, 1.0, 0.0},   // vert
        {0.0, 0.0, 0.0},   // noir
        {1.0, 0.0, 1.0},   // magenta
        {0.0, 1.0, 1.0},   // cyan
        {0.5, 0.5, 0.5},   // gris
    };

    cairo_set_line_width(cr, 2.0);

    for (int speed_idx = tws_from_idx; speed_idx <= tws_to_idx && speed_idx < data->num_speeds; speed_idx++) {
        int tws = data->tws_values[speed_idx];
        if (tws == 0) continue;  // Ne pas dessiner TWS 0

        int color_idx = speed_idx % 7;
        cairo_set_source_rgb(cr, colors[color_idx][0], colors[color_idx][1], colors[color_idx][2]);

        // Collecter les points tribord seulement (0° à 180°)
        double points_x[MAX_ANGLES];
        double points_y[MAX_ANGLES];
        int num_points = 0;

        for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
            if (!data->twa_present[angle_idx]) continue;
            double bsp = data->polar_data[angle_idx][speed_idx];
            if (bsp < 0.01) continue;

            double angle = data->twa_values[angle_idx];
            double rad = angle * M_PI / 180.0;
            double r = (bsp / max_scale) * radius;
            points_x[num_points] = center_x + r * sin(rad);
            points_y[num_points] = center_y - r * cos(rad);
            num_points++;
        }

        if (num_points < 2) continue;

        // Dessiner avec lissage Catmull-Rom
        cairo_new_path(cr);  // Commencer un nouveau chemin pour cette courbe
        cairo_move_to(cr, points_x[0], points_y[0]);

        for (int i = 0; i < num_points - 1; i++) {
            // Points actuels
            double x1 = points_x[i];
            double y1 = points_y[i];
            double x2 = points_x[i + 1];
            double y2 = points_y[i + 1];

            // Points voisins pour Catmull-Rom
            double x0, y0, x3, y3;

            if (i == 0) {
                // Premier segment : extrapoler le point précédent
                x0 = 2 * x1 - x2;
                y0 = 2 * y1 - y2;
            } else {
                x0 = points_x[i - 1];
                y0 = points_y[i - 1];
            }

            if (i == num_points - 2) {
                // Dernier segment : extrapoler le point suivant
                x3 = 2 * x2 - x1;
                y3 = 2 * y2 - y1;
            } else {
                x3 = points_x[i + 2];
                y3 = points_y[i + 2];
            }

            // Calcul des points de contrôle Catmull-Rom avec tension 0.5
            double tension = 0.5;
            double cp1x = x1 + (x2 - x0) * tension / 6.0;
            double cp1y = y1 + (y2 - y0) * tension / 6.0;
            double cp2x = x2 - (x3 - x1) * tension / 6.0;
            double cp2y = y2 - (y3 - y1) * tension / 6.0;

            cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
        }

        cairo_stroke(cr);
    }

    return FALSE;
}

// Recréer complètement l'onglet données (pour changement de colonnes)
void rebuild_data_tab(AppWidgets *app) {
    PolarData *data = app->polar_data;

    // Trouver le container scrolled window
    GtkWidget *data_tab = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), 0);
    if (!data_tab) return;

    // Détruire l'ancien grid
    GtkWidget *old_grid = gtk_bin_get_child(GTK_BIN(data_tab));
    if (old_grid) {
        gtk_widget_destroy(old_grid);
    }

    // Créer nouveau grid
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);

    // En-tête: TWA\TWS
    GtkWidget *corner_label = gtk_label_new("TWA\\TWS");
    gtk_widget_set_size_request(corner_label, 80, 30);
    gtk_grid_attach(GTK_GRID(grid), corner_label, 0, 0, 1, 1);

    // En-têtes de colonnes (TWS)
    for (int i = 0; i < data->num_speeds; i++) {
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "%d kn", data->tws_values[i]);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_size_request(label, 45, -1);  // Largeur fixe 45 pixels

        // Envelopper dans un EventBox pour détecter les clics
        GtkWidget *event_box = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(event_box), label);

        // Créer les données pour le callback
        HeaderData *header_data = g_new(HeaderData, 1);
        header_data->app = app;
        header_data->index = i;
        header_data->is_twa = FALSE;

        g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_header_clicked), header_data);
        g_object_set_data_full(G_OBJECT(event_box), "header-data", header_data, g_free);

        gtk_grid_attach(GTK_GRID(grid), event_box, i + 1, 0, 1, 1);
    }

    // Lignes de données - seulement celles présentes dans le fichier
    int row = 1;
    for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;  // Ignorer les lignes absentes

        int angle = data->twa_values[angle_idx];
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "%03d°", angle);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_size_request(label, 80, 30);

        // Envelopper dans un EventBox pour détecter les clics
        GtkWidget *event_box = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(event_box), label);

        // Créer les données pour le callback
        HeaderData *header_data = g_new(HeaderData, 1);
        header_data->app = app;
        header_data->index = angle_idx;
        header_data->is_twa = TRUE;

        g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_header_clicked), header_data);
        g_object_set_data_full(G_OBJECT(event_box), "header-data", header_data, g_free);

        gtk_grid_attach(GTK_GRID(grid), event_box, 0, row, 1, 1);

        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            GtkWidget *entry = gtk_entry_new();
            gtk_entry_set_width_chars(GTK_ENTRY(entry), 5);  // Largeur de 5 caractères
            gtk_entry_set_max_width_chars(GTK_ENTRY(entry), 5);
            gtk_entry_set_max_length(GTK_ENTRY(entry), 5);  // Format xx.xx (5 caractères max)
            gtk_entry_set_alignment(GTK_ENTRY(entry), 1.0);

            // Utiliser la valeur texte originale (pas de conversion/arrondi)
            gtk_entry_set_text(GTK_ENTRY(entry), data->polar_data_str[angle_idx][speed_idx]);

            // Créer une structure pour passer les coordonnées
            CellData *cell_data = g_new(CellData, 1);
            cell_data->app = app;
            cell_data->angle_idx = angle_idx;
            cell_data->speed_idx = speed_idx;

            // Connecter le signal de changement
            g_signal_connect(entry, "changed", G_CALLBACK(on_cell_changed), cell_data);
            g_object_set_data_full(G_OBJECT(entry), "cell-data", cell_data, g_free);

            gtk_grid_attach(GTK_GRID(grid), entry, speed_idx + 1, row, 1, 1);
        }
        row++;
    }

    gtk_container_add(GTK_CONTAINER(data_tab), grid);
    gtk_widget_show_all(grid);
    app->grid_table = grid;
}

// Mettre à jour l'onglet données (valeurs uniquement)
void update_data_tab(AppWidgets *app) {
    PolarData *data = app->polar_data;
    GtkWidget *grid = app->grid_table;

    if (!grid) return;

    // Parcourir les cellules et mettre à jour les valeurs
    for (int angle_idx = 0; angle_idx < MAX_ANGLES; angle_idx++) {
        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            GtkWidget *entry = gtk_grid_get_child_at(GTK_GRID(grid), speed_idx + 1, angle_idx + 1);
            if (entry && GTK_IS_ENTRY(entry)) {
                char value_text[32];
                double val = data->polar_data[angle_idx][speed_idx];
                // Afficher avec 2 décimales, mais enlever les zéros inutiles
                snprintf(value_text, sizeof(value_text), "%.2f", val);
                // Supprimer les zéros à la fin après la virgule
                char *dot = strchr(value_text, '.');
                if (dot) {
                    char *end = value_text + strlen(value_text) - 1;
                    while (end > dot && *end == '0') *end-- = '\0';
                    if (end == dot) *end = '\0';
                }
                gtk_entry_set_text(GTK_ENTRY(entry), value_text);
            }
        }
    }
}

// Créer l'onglet "Données de la polaire"
GtkWidget *create_data_tab(AppWidgets *app) {
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);

    PolarData *data = app->polar_data;

    // En-tête: TWA\TWS
    GtkWidget *corner_label = gtk_label_new("TWA\\TWS");
    gtk_widget_set_size_request(corner_label, 80, 30);
    gtk_grid_attach(GTK_GRID(grid), corner_label, 0, 0, 1, 1);

    // En-têtes de colonnes (TWS)
    for (int i = 0; i < data->num_speeds; i++) {
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "%d", data->tws_values[i]);  // Sans "kn" pour gagner de la place
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_size_request(label, 10, 30);  // Réduit de 60 à 50
        gtk_grid_attach(GTK_GRID(grid), label, i + 1, 0, 1, 1);
    }

    // Lignes de données - seulement celles présentes dans le fichier
    int row = 1;
    for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;  // Ignorer les lignes absentes

        int angle = data->twa_values[angle_idx];
        char label_text[32];
        snprintf(label_text, sizeof(label_text), "%03d°", angle);
        GtkWidget *label = gtk_label_new(label_text);
        gtk_widget_set_size_request(label, 80, 30);

        // Envelopper dans un EventBox pour détecter les clics
        GtkWidget *event_box = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(event_box), label);

        // Créer les données pour le callback
        HeaderData *header_data = g_new(HeaderData, 1);
        header_data->app = app;
        header_data->index = angle_idx;
        header_data->is_twa = TRUE;

        g_signal_connect(event_box, "button-press-event", G_CALLBACK(on_header_clicked), header_data);
        g_object_set_data_full(G_OBJECT(event_box), "header-data", header_data, g_free);

        gtk_grid_attach(GTK_GRID(grid), event_box, 0, row, 1, 1);

        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            GtkWidget *entry = gtk_entry_new();
            gtk_entry_set_width_chars(GTK_ENTRY(entry), 5);  // Largeur de 5 caractères
            gtk_entry_set_max_width_chars(GTK_ENTRY(entry), 5);
            gtk_entry_set_max_length(GTK_ENTRY(entry), 5);  // Format xx.xx (5 caractères max)
            gtk_entry_set_alignment(GTK_ENTRY(entry), 1.0);

            // Utiliser la valeur texte originale (pas de conversion/arrondi)
            gtk_entry_set_text(GTK_ENTRY(entry), data->polar_data_str[angle_idx][speed_idx]);

            // Créer une structure pour passer les coordonnées
            CellData *cell_data = g_new(CellData, 1);
            cell_data->app = app;
            cell_data->angle_idx = angle_idx;
            cell_data->speed_idx = speed_idx;

            // Connecter le signal de changement
            g_signal_connect(entry, "changed", G_CALLBACK(on_cell_changed), cell_data);
            g_object_set_data_full(G_OBJECT(entry), "cell-data", cell_data, g_free);

            gtk_grid_attach(GTK_GRID(grid), entry, speed_idx + 1, row, 1, 1);
        }
        row++;
    }

    gtk_container_add(GTK_CONTAINER(scrolled), grid);
    app->grid_table = grid;

    return scrolled;
}

// Créer l'onglet "Diagramme de la polaire"
GtkWidget *create_diagram_tab(AppWidgets *app) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);

    // Zone de dessin
    GtkWidget *drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, 600, 600);
    g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(draw_polar_diagram), app);
    gtk_box_pack_start(GTK_BOX(hbox), drawing_area, TRUE, TRUE, 0);

    // Panneau latéral droit
    GtkWidget *vbox_right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    // Sélecteurs de TWS
    GtkWidget *selector_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    app->tws_from_label = gtk_label_new(TR(app, "Voir les polaires pour les TWS de", "View polars for TWS from"));
    gtk_box_pack_start(GTK_BOX(selector_box), app->tws_from_label, FALSE, FALSE, 0);

    app->tws_from_combo = gtk_combo_box_text_new();
    app->tws_to_combo = gtk_combo_box_text_new();

    for (int i = 0; i < app->polar_data->num_speeds; i++) {
        int tws = app->polar_data->tws_values[i];
        if (tws == 0) continue;  // Ne pas afficher TWS 0
        char text[16];
        snprintf(text, sizeof(text), "%d kn", tws);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), 6);

    // Connecter les signaux de changement
    g_signal_connect(app->tws_from_combo, "changed", G_CALLBACK(on_tws_changed), app);
    g_signal_connect(app->tws_to_combo, "changed", G_CALLBACK(on_tws_changed), app);

    gtk_box_pack_start(GTK_BOX(selector_box), app->tws_from_combo, FALSE, FALSE, 0);
    app->tws_to_label = gtk_label_new(TR(app, "à", "to"));
    gtk_box_pack_start(GTK_BOX(selector_box), app->tws_to_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selector_box), app->tws_to_combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox_right), selector_box, FALSE, FALSE, 0);

    // Container pour le tableau VMG (sera rempli dynamiquement)
    app->vmg_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox_right), app->vmg_container, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox_right, FALSE, FALSE, 0);

    app->polar_view = drawing_area;

    return hbox;
}

// Reconstruire le tableau VMG
void rebuild_vmg_table(AppWidgets *app) {
    PolarData *data = app->polar_data;

    // Supprimer l'ancien contenu
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->vmg_container));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);

    // Créer nouveau tableau VMG
    GtkWidget *vmg_frame = gtk_frame_new("VMG (Velocity Made Good)");
    GtkWidget *vmg_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vmg_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(vmg_scroll, -1, 300);

    GtkWidget *vmg_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(vmg_grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(vmg_grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(vmg_grid), 10);

    // En-têtes
    GtkWidget *hdr_tws = gtk_label_new("TWS");
    GtkWidget *hdr_vmg_up = gtk_label_new("max VMG ↑");
    GtkWidget *hdr_vmg_down = gtk_label_new("max VMG ↓");

    PangoAttrList *bold_attrs = pango_attr_list_new();
    pango_attr_list_insert(bold_attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(hdr_tws), bold_attrs);
    gtk_label_set_attributes(GTK_LABEL(hdr_vmg_up), bold_attrs);
    gtk_label_set_attributes(GTK_LABEL(hdr_vmg_down), bold_attrs);
    pango_attr_list_unref(bold_attrs);

    gtk_grid_attach(GTK_GRID(vmg_grid), hdr_tws, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(vmg_grid), hdr_vmg_up, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(vmg_grid), hdr_vmg_down, 2, 0, 1, 1);

    // Calculer VMG pour chaque TWS affiché
    int combo_from = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_from_combo));
    int combo_to = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_to_combo));

    // Convertir les indices de combo box vers les indices réels
    int tws_from_idx = (combo_from >= 0) ? combo_index_to_tws_index(app->polar_data, combo_from) : 0;
    int tws_to_idx = (combo_to >= 0) ? combo_index_to_tws_index(app->polar_data, combo_to) : app->polar_data->num_speeds - 1;

    double colors[][3] = {
        {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 0.0},
        {1.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {0.5, 0.5, 0.5}
    };

    int row = 1;
    for (int speed_idx = tws_from_idx; speed_idx <= tws_to_idx && speed_idx < app->polar_data->num_speeds; speed_idx++) {
        int tws = app->polar_data->tws_values[speed_idx];
        if (tws == 0) continue;  // Ne pas afficher TWS 0 dans le tableau VMG

        // Calculer VMG upwind (angles 0-90°, VMG = BSP * cos(TWA))
        // Interpoler entre les angles pour trouver le vrai maximum
        double max_vmg_up = 0.0;
        double best_twa_up = 0.0;
        double best_bsp_up = 0.0;

        // Parcourir tous les segments entre angles présents
        for (int idx1 = 0; idx1 < app->polar_data->num_angles - 1; idx1++) {
            if (!app->polar_data->twa_present[idx1]) continue;

            // Trouver le prochain angle présent
            int idx2 = -1;
            for (int i = idx1 + 1; i < app->polar_data->num_angles; i++) {
                if (app->polar_data->twa_present[i]) {
                    idx2 = i;
                    break;
                }
            }
            if (idx2 == -1) continue;

            double twa1 = app->polar_data->twa_values[idx1];
            double twa2 = app->polar_data->twa_values[idx2];

            // Ne traiter que les angles upwind (0-90°)
            if (twa1 > 90.0) break;
            if (twa2 < 0.0) continue;

            double bsp1 = app->polar_data->polar_data[idx1][speed_idx];
            double bsp2 = app->polar_data->polar_data[idx2][speed_idx];

            if (bsp1 < 0.01 && bsp2 < 0.01) continue;

            // Interpoler avec une résolution de 0.1° dans ce segment
            double start_twa = (twa1 < 0.0) ? 0.0 : twa1;
            double end_twa = (twa2 > 90.0) ? 90.0 : twa2;

            for (double twa = start_twa; twa <= end_twa; twa += 0.1) {
                // Interpolation linéaire
                double t = (twa - twa1) / (twa2 - twa1);
                double bsp = bsp1 + t * (bsp2 - bsp1);

                double vmg = bsp * cos(twa * M_PI / 180.0);
                if (vmg > max_vmg_up) {
                    max_vmg_up = vmg;
                    best_twa_up = twa;
                    best_bsp_up = bsp;
                }
            }
        }

        // Calculer VMG downwind (angles 90-180°, VMG = -BSP * cos(TWA))
        // Interpoler entre les angles pour trouver le vrai maximum
        double max_vmg_down = 0.0;
        double best_twa_down = 0.0;
        double best_bsp_down = 0.0;

        // Parcourir tous les segments entre angles présents
        for (int idx1 = 0; idx1 < app->polar_data->num_angles - 1; idx1++) {
            if (!app->polar_data->twa_present[idx1]) continue;

            // Trouver le prochain angle présent
            int idx2 = -1;
            for (int i = idx1 + 1; i < app->polar_data->num_angles; i++) {
                if (app->polar_data->twa_present[i]) {
                    idx2 = i;
                    break;
                }
            }
            if (idx2 == -1) continue;

            double twa1 = app->polar_data->twa_values[idx1];
            double twa2 = app->polar_data->twa_values[idx2];

            // Ne traiter que les angles downwind (90-180°)
            if (twa2 < 90.0) continue;
            if (twa1 > 180.0) break;

            double bsp1 = app->polar_data->polar_data[idx1][speed_idx];
            double bsp2 = app->polar_data->polar_data[idx2][speed_idx];

            if (bsp1 < 0.01 && bsp2 < 0.01) continue;

            // Interpoler avec une résolution de 0.1° dans ce segment
            double start_twa = (twa1 < 90.0) ? 90.0 : twa1;
            double end_twa = (twa2 > 180.0) ? 180.0 : twa2;

            for (double twa = start_twa; twa <= end_twa; twa += 0.1) {
                // Interpolation linéaire
                double t = (twa - twa1) / (twa2 - twa1);
                double bsp = bsp1 + t * (bsp2 - bsp1);

                double vmg = -bsp * cos(twa * M_PI / 180.0);

                if (vmg > max_vmg_down) {
                    max_vmg_down = vmg;
                    best_twa_down = twa;
                    best_bsp_down = bsp;
                }
            }
        }

        // Créer les labels avec couleur
        char tws_text[32];
        snprintf(tws_text, sizeof(tws_text), "  %d kn", tws);
        GtkWidget *tws_label = gtk_label_new(tws_text);

        // Appliquer la couleur
        int color_idx = speed_idx % 7;
        char color_markup[256];
        snprintf(color_markup, sizeof(color_markup),
                 "<span foreground=\"#%02x%02x%02x\">━━</span> %d kn",
                 (int)(colors[color_idx][0] * 255),
                 (int)(colors[color_idx][1] * 255),
                 (int)(colors[color_idx][2] * 255),
                 tws);
        GtkWidget *colored_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(colored_label), color_markup);
        gtk_widget_set_halign(colored_label, GTK_ALIGN_START);

        char vmg_up_text[64], vmg_down_text[64];

        if (max_vmg_up > 0.01) {
            snprintf(vmg_up_text, sizeof(vmg_up_text), "%05.1f°/%.1f kn", best_twa_up, max_vmg_up);
        } else {
            strcpy(vmg_up_text, "—");
        }

        if (max_vmg_down > 0.01) {
            snprintf(vmg_down_text, sizeof(vmg_down_text), "%05.1f°/%.1f kn", best_twa_down, max_vmg_down);
        } else {
            strcpy(vmg_down_text, "—");
        }

        GtkWidget *vmg_up_label = gtk_label_new(vmg_up_text);
        GtkWidget *vmg_down_label = gtk_label_new(vmg_down_text);

        gtk_widget_set_halign(colored_label, GTK_ALIGN_START);
        gtk_widget_set_halign(vmg_up_label, GTK_ALIGN_START);
        gtk_widget_set_halign(vmg_down_label, GTK_ALIGN_START);

        gtk_grid_attach(GTK_GRID(vmg_grid), colored_label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(vmg_grid), vmg_up_label, 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(vmg_grid), vmg_down_label, 2, row, 1, 1);

        row++;
    }

    gtk_container_add(GTK_CONTAINER(vmg_scroll), vmg_grid);
    gtk_container_add(GTK_CONTAINER(vmg_frame), vmg_scroll);
    gtk_box_pack_start(GTK_BOX(app->vmg_container), vmg_frame, TRUE, TRUE, 0);

    gtk_widget_show_all(app->vmg_container);
}

// Callback pour changement de valeur dans une cellule
void on_cell_changed(GtkEntry *entry, gpointer user_data) {
    CellData *cell_data = (CellData *)user_data;
    AppWidgets *app = cell_data->app;
    PolarData *data = app->polar_data;

    const char *text = gtk_entry_get_text(entry);

    // Mettre à jour la valeur texte
    strncpy(data->polar_data_str[cell_data->angle_idx][cell_data->speed_idx], text, 15);
    data->polar_data_str[cell_data->angle_idx][cell_data->speed_idx][15] = '\0';

    // Mettre à jour la valeur numérique pour le diagramme
    data->polar_data[cell_data->angle_idx][cell_data->speed_idx] = atof(text);

    // Marquer comme modifié
    data->modified = TRUE;

    // Recalculer le tableau VMG
    rebuild_vmg_table(app);

    // Rafraîchir le diagramme
    gtk_widget_queue_draw(app->polar_view);
}

// Callback pour changement de TWS
void on_tws_changed(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    rebuild_vmg_table(app);
    gtk_widget_queue_draw(app->polar_view);
}

// Demander si l'utilisateur veut enregistrer les modifications
gboolean prompt_save_changes(AppWidgets *app) {
    if (!app->polar_data->modified) return TRUE;  // Pas de modifications, continuer

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_NONE,
                                                "%s", TR(app, "Voulez-vous enregistrer les modifications ?", "Do you want to save changes?"));
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                           TR(app, "_Ne pas enregistrer", "_Don't Save"), GTK_RESPONSE_NO,
                           TR(app, "_Enregistrer", "_Save"), GTK_RESPONSE_YES,
                           NULL);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_CANCEL) {
        return FALSE;  // Annuler l'action
    } else if (response == GTK_RESPONSE_YES) {
        // Ouvrir dialogue "Enregistrer sous"
        char *filename = NULL;

#ifdef _WIN32
        // Utiliser dialogue natif Windows
        const char *default_name = (strlen(app->polar_data->filename) > 0)
                                    ? app->polar_data->filename
                                    : "polaire.pol";
        filename = win32_save_dialog(app->window,
                                     TR(app, "Enregistrer la polaire", "Save Polar"),
                                     TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"),
                                     "*.pol",
                                     default_name);
#else
        GtkWidget *save_dialog = gtk_file_chooser_dialog_new(TR(app, "Enregistrer la polaire", "Save Polar"),
                                                              GTK_WINDOW(app->window),
                                                              GTK_FILE_CHOOSER_ACTION_SAVE,
                                                              TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                              TR(app, "_Enregistrer", "_Save"), GTK_RESPONSE_ACCEPT,
                                                              NULL);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(save_dialog), TRUE);

        // Ajouter un filtre pour les fichiers .pol
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"));
        gtk_file_filter_add_pattern(filter, "*.pol");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(save_dialog), filter);

        // Si un fichier est déjà ouvert, utiliser son nom par défaut
        if (strlen(app->polar_data->filename) > 0) {
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(save_dialog), app->polar_data->filename);
        } else {
            // Proposer un nom par défaut
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dialog), "polaire.pol");
        }

        if (gtk_dialog_run(GTK_DIALOG(save_dialog)) == GTK_RESPONSE_ACCEPT) {
            filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(save_dialog));
        }
        gtk_widget_destroy(save_dialog);
#endif

        if (filename) {
            save_polar_file(filename, app->polar_data);
            gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, filename);
#ifdef _WIN32
            free(filename);
#else
            g_free(filename);
#endif
        } else {
            // L'utilisateur a annulé l'enregistrement
            return FALSE;  // Annuler l'action principale
        }
    }

    return TRUE;  // Continuer l'action
}

// Callbacks des menus
void on_open_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Demander si l'utilisateur veut enregistrer les modifications
    if (!prompt_save_changes(app)) return;

    char *filename = NULL;

#ifdef _WIN32
    // Utiliser dialogue natif Windows
    filename = win32_open_dialog(app->window,
                                 TR(app, "Ouvrir une polaire", "Open Polar"),
                                 TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"),
                                 "*.pol");
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(TR(app, "Ouvrir une polaire", "Open Polar"),
                                                     GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                     TR(app, "_Ouvrir", "_Open"), GTK_RESPONSE_ACCEPT,
                                                     NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"));
    gtk_file_filter_add_pattern(filter, "*.pol");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);
#endif

    if (filename) {

        if (load_polar_file(filename, app->polar_data)) {
            // Reconstruire complètement l'interface (les colonnes peuvent changer)
            rebuild_data_tab(app);

            // Mettre à jour les combo box TWS
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_from_combo));
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_to_combo));

            for (int i = 0; i < app->polar_data->num_speeds; i++) {
                int tws = app->polar_data->tws_values[i];
                if (tws == 0) continue;  // Ne pas afficher TWS 0
                char text[16];
                snprintf(text, sizeof(text), "%d kn", tws);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text);
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
            int last_idx = app->polar_data->num_speeds > 6 ? 6 : app->polar_data->num_speeds - 1;
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);

            // Reconstruire le tableau VMG
            rebuild_vmg_table(app);

            gtk_widget_queue_draw(app->polar_view);
            gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, filename);
        } else {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "Impossible d'ouvrir le fichier", "Unable to open file"));
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }

#ifdef _WIN32
        free(filename);
#else
        g_free(filename);
#endif
    }
}

void on_save_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    char *filename = NULL;

#ifdef _WIN32
    // Utiliser dialogue natif Windows
    const char *default_name = (strlen(app->polar_data->filename) > 0)
                                ? app->polar_data->filename
                                : "polar.pol";
    filename = win32_save_dialog(app->window,
                                 TR(app, "Enregistrer la polaire", "Save Polar"),
                                 TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"),
                                 "*.pol",
                                 default_name);
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(TR(app, "Enregistrer la polaire", "Save Polar"),
                                                     GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_SAVE,
                                                     TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                     TR(app, "_Enregistrer", "_Save"), GTK_RESPONSE_ACCEPT,
                                                     NULL);

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    // Ajouter filtre .pol
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, TR(app, "Fichiers polaires (*.pol)", "Polar files (*.pol)"));
    gtk_file_filter_add_pattern(filter, "*.pol");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, TR(app, "Tous les fichiers", "All files"));
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    // Si un fichier existe déjà, le proposer par défaut
    if (strlen(app->polar_data->filename) > 0) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), app->polar_data->filename);
    } else {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "polar.pol");
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);
#endif

    if (filename) {
        save_polar_file(filename, app->polar_data);
        gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, app->polar_data->filename);
#ifdef _WIN32
        free(filename);
#else
        g_free(filename);
#endif
    }
}

void on_print_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    GtkPrintOperation *print = gtk_print_operation_new();

    // Callback pour dessiner la page
    g_signal_connect(print, "draw-page", G_CALLBACK(print_page), app);
    g_signal_connect(print, "begin-print", G_CALLBACK(print_begin), app);

    GError *error = NULL;
    GtkPrintOperationResult result = gtk_print_operation_run(print,
                                                              GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                                              GTK_WINDOW(app->window),
                                                              &error);

    if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
        GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                          GTK_DIALOG_MODAL,
                                                          GTK_MESSAGE_ERROR,
                                                          GTK_BUTTONS_OK,
                                                          "%s%s",
                                                          TR(app, "Erreur d'impression: ", "Print error: "),
                                                          error->message);
        gtk_dialog_run(GTK_DIALOG(error_dialog));
        gtk_widget_destroy(error_dialog);
        g_error_free(error);
    }

    g_object_unref(print);
}

void print_begin(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data) {
    gtk_print_operation_set_n_pages(operation, 1);
}

void print_page(GtkPrintOperation *operation, GtkPrintContext *context, gint page_nr, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    PolarData *data = app->polar_data;
    cairo_t *cr = gtk_print_context_get_cairo_context(context);

    double width = gtk_print_context_get_width(context);
    double height = gtk_print_context_get_height(context);

    // Diviser la page: données en haut (40%), diagramme en bas (60%)
    double data_height = height * 0.40;
    double diagram_height = height * 0.60;

    // Fond blanc
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // === PARTIE 1: Tableau de données ===
    cairo_save(cr);

    // Titre avec nom du fichier sans extension .pol
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, 10, 15);

    // Extraire le nom du fichier sans chemin et sans extension .pol
    char title[256];
    if (strlen(data->filename) > 0) {
        const char *basename = strrchr(data->filename, '/');
        basename = basename ? basename + 1 : data->filename;
        strncpy(title, basename, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';

        // Enlever l'extension .pol si présente
        char *dot = strrchr(title, '.');
        if (dot && strcasecmp(dot, ".pol") == 0) {
            *dot = '\0';
        }
    } else {
        snprintf(title, sizeof(title), TR(app, "Polaire sans nom", "Unnamed Polar"));
    }
    cairo_show_text(cr, title);

    // Calculer dimensions du tableau
    int num_cols = data->num_speeds + 1;  // +1 pour TWA
    int num_rows = 0;
    for (int i = 0; i < data->num_angles; i++) {
        if (data->twa_present[i]) num_rows++;
    }
    num_rows++;  // +1 pour en-tête

    double first_col_width = 40;  // Largeur pour la colonne TWA
    double col_width = (width - first_col_width) / data->num_speeds;
    if (col_width > 50) col_width = 50;  // Max 50 points par colonne
    double row_height = 12;

    double table_width = first_col_width + col_width * data->num_speeds;
    double start_x = (width - table_width) / 2;
    double start_y = 25;

    // Limiter le nombre de lignes pour tenir dans data_height
    int max_rows = (int)((data_height - start_y - 10) / row_height);
    if (num_rows > max_rows) num_rows = max_rows;

    cairo_set_font_size(cr, 7);

    // En-tête
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_rectangle(cr, start_x, start_y, table_width, row_height);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_move_to(cr, start_x + 2, start_y + row_height - 3);
    cairo_show_text(cr, "TWA\\TWS");

    for (int i = 0; i < data->num_speeds; i++) {
        char text[16];
        snprintf(text, sizeof(text), "%d kn", data->tws_values[i]);  // Ajouter "kn"
        cairo_move_to(cr, start_x + first_col_width + i * col_width + 2, start_y + row_height - 3);
        cairo_show_text(cr, text);
    }

    // Données (limiter au nombre de lignes disponibles)
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    int row = 1;
    for (int angle_idx = 0; angle_idx < data->num_angles && row < num_rows; angle_idx++) {
        if (!data->twa_present[angle_idx]) continue;

        double y = start_y + row * row_height;

        // Ligne alternée
        if (row % 2 == 0) {
            cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
            cairo_rectangle(cr, start_x, y, table_width, row_height);
            cairo_fill(cr);
        }

        // TWA
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        char text[16];
        snprintf(text, sizeof(text), "%d°", data->twa_values[angle_idx]);
        cairo_move_to(cr, start_x + 2, y + row_height - 3);
        cairo_show_text(cr, text);

        // BSP values
        for (int speed_idx = 0; speed_idx < data->num_speeds; speed_idx++) {
            snprintf(text, sizeof(text), "%s", data->polar_data_str[angle_idx][speed_idx]);
            cairo_move_to(cr, start_x + first_col_width + speed_idx * col_width + 2, y + row_height - 3);
            cairo_show_text(cr, text);
        }

        row++;
    }

    // Grille du tableau
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 0.5);

    // Lignes horizontales
    for (int i = 0; i <= row; i++) {
        double y = start_y + i * row_height;
        cairo_move_to(cr, start_x, y);
        cairo_line_to(cr, start_x + table_width, y);
        cairo_stroke(cr);
    }

    // Lignes verticales
    // Première ligne verticale (bord gauche)
    cairo_move_to(cr, start_x, start_y);
    cairo_line_to(cr, start_x, start_y + row * row_height);
    cairo_stroke(cr);

    // Ligne après la première colonne TWA
    cairo_move_to(cr, start_x + first_col_width, start_y);
    cairo_line_to(cr, start_x + first_col_width, start_y + row * row_height);
    cairo_stroke(cr);

    // Lignes pour les autres colonnes
    for (int i = 1; i <= data->num_speeds; i++) {
        double x = start_x + first_col_width + i * col_width;
        cairo_move_to(cr, x, start_y);
        cairo_line_to(cr, x, start_y + row * row_height);
        cairo_stroke(cr);
    }

    cairo_restore(cr);

    // === PARTIE 2: Diagramme polaire ===
    cairo_save(cr);
    cairo_translate(cr, 0, data_height);

    // Titre
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, 10, 15);
    cairo_show_text(cr, TR(app, "Diagramme de la polaire", "Polar Diagram"));

    // Dessiner le diagramme (similaire à draw_polar_diagram mais adapté pour impression)
    int margin = 40;
    int center_x = margin + 20;
    int center_y = diagram_height / 2;
    int radius = (diagram_height / 2) - margin - 20;

    // Fond blanc
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_rectangle(cr, 0, 20, width, diagram_height - 20);
    cairo_fill(cr);

    // Demi-cercle de fond
    cairo_set_source_rgb(cr, 0.98, 0.98, 0.98);
    cairo_move_to(cr, center_x, center_y - radius);
    cairo_arc(cr, center_x, center_y, radius, -M_PI / 2, M_PI / 2);
    cairo_line_to(cr, center_x, center_y);
    cairo_close_path(cr);
    cairo_fill(cr);

    // Récupérer les TWS sélectionnés
    int combo_from = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_from_combo));
    int combo_to = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_to_combo));

    // Convertir les indices de combo box vers les indices réels
    int tws_from_idx = (combo_from >= 0) ? combo_index_to_tws_index(data, combo_from) : 0;
    int tws_to_idx = (combo_to >= 0) ? combo_index_to_tws_index(data, combo_to) : data->num_speeds - 1;

    // Trouver max BSP
    double max_bsp = 0.0;
    for (int i = 0; i < MAX_ANGLES; i++) {
        for (int j = tws_from_idx; j <= tws_to_idx && j < data->num_speeds; j++) {
            if (data->polar_data[i][j] > max_bsp) {
                max_bsp = data->polar_data[i][j];
            }
        }
    }
    int max_scale = (int)ceil(max_bsp);
    if (max_scale < 1) max_scale = 10;

    // Grille circulaire
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_line_width(cr, 0.5);

    int step = max_scale > 10 ? 2 : 1;
    for (int i = step; i <= max_scale; i += step) {
        double r = radius * i / (double)max_scale;
        cairo_new_path(cr);
        cairo_arc(cr, center_x, center_y, r, -M_PI / 2, M_PI / 2);
        cairo_stroke(cr);

        // Labels BSP
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
        cairo_set_font_size(cr, 7);
        char label[16];
        snprintf(label, sizeof(label), "%d", i);
        cairo_move_to(cr, center_x + 1, center_y - r - 2);
        cairo_show_text(cr, label);
    }

    // Lignes radiales
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    for (int angle = 0; angle <= 180; angle += 15) {
        double rad = angle * M_PI / 180.0;
        cairo_move_to(cr, center_x, center_y);
        cairo_line_to(cr, center_x + radius * sin(rad), center_y - radius * cos(rad));
        cairo_stroke(cr);

        // Labels TWA
        if (angle % 15 == 0) {
            cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
            cairo_set_font_size(cr, 8);
            char angle_label[16];
            snprintf(angle_label, sizeof(angle_label), "%d°", angle);
            double label_r = radius + 15;
            double label_x = center_x + label_r * sin(rad);
            double label_y = center_y - label_r * cos(rad);
            cairo_move_to(cr, label_x - 8, label_y + 3);
            cairo_show_text(cr, angle_label);
        }
    }

    // Courbes polaires
    double colors[][3] = {
        {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 0.0},
        {1.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {0.5, 0.5, 0.5}
    };

    cairo_set_line_width(cr, 1.5);

    for (int speed_idx = tws_from_idx; speed_idx <= tws_to_idx && speed_idx < data->num_speeds; speed_idx++) {
        int tws = data->tws_values[speed_idx];
        if (tws == 0) continue;

        int color_idx = speed_idx % 7;
        cairo_set_source_rgb(cr, colors[color_idx][0], colors[color_idx][1], colors[color_idx][2]);

        double points_x[MAX_ANGLES];
        double points_y[MAX_ANGLES];
        int num_points = 0;

        for (int angle_idx = 0; angle_idx < data->num_angles; angle_idx++) {
            if (!data->twa_present[angle_idx]) continue;
            double bsp = data->polar_data[angle_idx][speed_idx];
            if (bsp < 0.01) continue;

            double angle = data->twa_values[angle_idx];
            double rad = angle * M_PI / 180.0;
            double r = (bsp / max_scale) * radius;
            points_x[num_points] = center_x + r * sin(rad);
            points_y[num_points] = center_y - r * cos(rad);
            num_points++;
        }

        if (num_points < 2) continue;

        // Dessiner avec lissage Catmull-Rom
        cairo_new_path(cr);
        cairo_move_to(cr, points_x[0], points_y[0]);

        for (int i = 0; i < num_points - 1; i++) {
            // Points actuels
            double x1 = points_x[i];
            double y1 = points_y[i];
            double x2 = points_x[i + 1];
            double y2 = points_y[i + 1];

            // Points voisins pour Catmull-Rom
            double x0, y0, x3, y3;

            if (i == 0) {
                // Premier segment : extrapoler le point précédent
                x0 = 2 * x1 - x2;
                y0 = 2 * y1 - y2;
            } else {
                x0 = points_x[i - 1];
                y0 = points_y[i - 1];
            }

            if (i == num_points - 2) {
                // Dernier segment : extrapoler le point suivant
                x3 = 2 * x2 - x1;
                y3 = 2 * y2 - y1;
            } else {
                x3 = points_x[i + 2];
                y3 = points_y[i + 2];
            }

            // Calcul des points de contrôle Catmull-Rom avec tension 0.5
            double tension = 0.5;
            double cp1x = x1 + (x2 - x0) * tension / 6.0;
            double cp1y = y1 + (y2 - y0) * tension / 6.0;
            double cp2x = x2 - (x3 - x1) * tension / 6.0;
            double cp2y = y2 - (y3 - y1) * tension / 6.0;

            cairo_curve_to(cr, cp1x, cp1y, cp2x, cp2y, x2, y2);
        }

        cairo_stroke(cr);
    }

    // === PARTIE 3: Tableau VMG à droite du diagramme ===
    double vmg_x = center_x + radius + 50;  // Position à droite du diagramme
    double vmg_y = 40;

    // Titre VMG
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_show_text(cr, "VMG (Velocity Made Good)");

    vmg_y += 15;

    // En-têtes du tableau VMG
    cairo_set_font_size(cr, 7);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_show_text(cr, "TWS (kn)");
    cairo_move_to(cr, vmg_x + 40, vmg_y);
    cairo_show_text(cr, TR(app, "max VMG up", "max VMG up"));
    cairo_move_to(cr, vmg_x + 110, vmg_y);
    cairo_show_text(cr, TR(app, "max VMG down", "max VMG down"));

    vmg_y += 10;

    // Ligne de séparation
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_line_to(cr, vmg_x + 180, vmg_y);
    cairo_stroke(cr);

    vmg_y += 8;  // Plus d'espace après la ligne de séparation

    // Calculer et afficher VMG pour chaque TWS
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 7);

    for (int speed_idx = tws_from_idx; speed_idx <= tws_to_idx && speed_idx < data->num_speeds; speed_idx++) {
        int tws = data->tws_values[speed_idx];
        if (tws == 0) continue;

        // Calculer VMG upwind
        double max_vmg_up = 0.0;
        double best_twa_up = 0.0;

        for (int idx1 = 0; idx1 < data->num_angles - 1; idx1++) {
            if (!data->twa_present[idx1]) continue;

            int idx2 = -1;
            for (int i = idx1 + 1; i < data->num_angles; i++) {
                if (data->twa_present[i]) {
                    idx2 = i;
                    break;
                }
            }
            if (idx2 == -1) continue;

            double twa1 = data->twa_values[idx1];
            double twa2 = data->twa_values[idx2];

            if (twa1 > 90.0) break;
            if (twa2 < 0.0) continue;

            double bsp1 = data->polar_data[idx1][speed_idx];
            double bsp2 = data->polar_data[idx2][speed_idx];

            if (bsp1 < 0.01 && bsp2 < 0.01) continue;

            double start_twa = (twa1 < 0.0) ? 0.0 : twa1;
            double end_twa = (twa2 > 90.0) ? 90.0 : twa2;

            for (double twa = start_twa; twa <= end_twa; twa += 0.1) {
                double t = (twa - twa1) / (twa2 - twa1);
                double bsp = bsp1 + t * (bsp2 - bsp1);
                double vmg = bsp * cos(twa * M_PI / 180.0);
                if (vmg > max_vmg_up) {
                    max_vmg_up = vmg;
                    best_twa_up = twa;
                }
            }
        }

        // Calculer VMG downwind
        double max_vmg_down = 0.0;
        double best_twa_down = 0.0;

        for (int idx1 = 0; idx1 < data->num_angles - 1; idx1++) {
            if (!data->twa_present[idx1]) continue;

            int idx2 = -1;
            for (int i = idx1 + 1; i < data->num_angles; i++) {
                if (data->twa_present[i]) {
                    idx2 = i;
                    break;
                }
            }
            if (idx2 == -1) continue;

            double twa1 = data->twa_values[idx1];
            double twa2 = data->twa_values[idx2];

            if (twa2 < 90.0) continue;
            if (twa1 > 180.0) break;

            double bsp1 = data->polar_data[idx1][speed_idx];
            double bsp2 = data->polar_data[idx2][speed_idx];

            if (bsp1 < 0.01 && bsp2 < 0.01) continue;

            double start_twa = (twa1 < 90.0) ? 90.0 : twa1;
            double end_twa = (twa2 > 180.0) ? 180.0 : twa2;

            for (double twa = start_twa; twa <= end_twa; twa += 0.1) {
                double t = (twa - twa1) / (twa2 - twa1);
                double bsp = bsp1 + t * (bsp2 - bsp1);
                double vmg = -bsp * cos(twa * M_PI / 180.0);
                if (vmg > max_vmg_down) {
                    max_vmg_down = vmg;
                    best_twa_down = twa;
                }
            }
        }

        // Afficher la ligne VMG avec couleur
        int color_idx = speed_idx % 7;
        cairo_set_source_rgb(cr, colors[color_idx][0], colors[color_idx][1], colors[color_idx][2]);

        char tws_text[16];
        snprintf(tws_text, sizeof(tws_text), "%d", tws);
        cairo_move_to(cr, vmg_x, vmg_y);
        cairo_show_text(cr, tws_text);

        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

        if (max_vmg_up > 0.01) {
            char vmg_up_text[32];
            snprintf(vmg_up_text, sizeof(vmg_up_text), "%.1f°/%.1f kn", best_twa_up, max_vmg_up);
            cairo_move_to(cr, vmg_x + 40, vmg_y);
            cairo_show_text(cr, vmg_up_text);
        }

        if (max_vmg_down > 0.01) {
            char vmg_down_text[32];
            snprintf(vmg_down_text, sizeof(vmg_down_text), "%.1f°/%.1f kn", best_twa_down, max_vmg_down);
            cairo_move_to(cr, vmg_x + 110, vmg_y);
            cairo_show_text(cr, vmg_down_text);
        }

        vmg_y += 10;
    }

    cairo_restore(cr);
}

void on_create_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Demander confirmation si des modifications non sauvegardées
    if (!prompt_save_changes(app)) return;

    GSList *filenames = NULL;

#ifdef _WIN32
    // Utiliser dialogue natif Windows
    filenames = win32_open_multi_dialog(app->window,
                                       TR(app, "Sélectionner fichier(s) NMEA ou VDR", "Select NMEA or VDR file(s)"),
                                       TR(app, "Fichiers NMEA et VDR", "NMEA and VDR files"),
                                       "*.txt;*.nmea;*.log;*.db");
#else
    // Dialogue pour choisir le(s) fichier(s) de données
    GtkWidget *dialog = gtk_file_chooser_dialog_new(TR(app, "Sélectionner fichier(s) NMEA ou VDR", "Select NMEA or VDR file(s)"),
                                                     GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                     TR(app, "_Ouvrir", "_Open"), GTK_RESPONSE_ACCEPT,
                                                     NULL);

    // Permettre la sélection multiple
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    // Filtres de fichiers - Filtre combiné NMEA et VDR par défaut
    GtkFileFilter *filter_data = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_data, TR(app, "Fichiers NMEA et VDR", "NMEA and VDR files"));
    gtk_file_filter_add_pattern(filter_data, "*.txt");
    gtk_file_filter_add_pattern(filter_data, "*.nmea");
    gtk_file_filter_add_pattern(filter_data, "*.log");
    gtk_file_filter_add_pattern(filter_data, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_data);

    GtkFileFilter *filter_nmea = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_nmea, TR(app, "Fichiers NMEA (*.txt, *.nmea, *.log)", "NMEA files (*.txt, *.nmea, *.log)"));
    gtk_file_filter_add_pattern(filter_nmea, "*.txt");
    gtk_file_filter_add_pattern(filter_nmea, "*.nmea");
    gtk_file_filter_add_pattern(filter_nmea, "*.log");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_nmea);

    GtkFileFilter *filter_vdr = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_vdr, TR(app, "Fichiers VDR (*.db)", "VDR files (*.db)"));
    gtk_file_filter_add_pattern(filter_vdr, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_vdr);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, TR(app, "Tous les fichiers", "All files"));
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    // Définir le filtre par défaut
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter_data);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);
#endif

    if (filenames) {

        // Créer le dialogue de progression
        GtkWidget *progress_dialog = gtk_dialog_new();
        gtk_window_set_title(GTK_WINDOW(progress_dialog), "Génération de la polaire");
        gtk_window_set_transient_for(GTK_WINDOW(progress_dialog), GTK_WINDOW(app->window));
        gtk_window_set_modal(GTK_WINDOW(progress_dialog), TRUE);
        GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(progress_dialog));
        GtkWidget *progress_label = gtk_label_new("Initialisation...");
        gtk_box_pack_start(GTK_BOX(content_area), progress_label, TRUE, TRUE, 10);
        gtk_widget_show_all(progress_dialog);

        gboolean cancel_flag = FALSE;
        ProgressContext progress = {progress_dialog, progress_label, &cancel_flag};

        // Initialiser la grille polaire
        polar_grid_t grid;
        init_polar_grid(&grid);

        // Traiter tous les fichiers sélectionnés
        int total_result = 0;
        int file_count = 0;
        for (GSList *l = filenames; l != NULL; l = l->next) {
            char *filename = (char *)l->data;
            file_count++;

            char msg[256];
            snprintf(msg, sizeof(msg), "%s %d/%d...",
                     TR(app, "Traitement du fichier", "Processing file"),
                     file_count, g_slist_length(filenames));
            gtk_label_set_text(GTK_LABEL(progress_label), msg);
            while (gtk_events_pending()) gtk_main_iteration();

            int result = process_file(filename, &grid, false, &progress);
            if (result > 0) total_result += result;
        }

        int result = total_result;

        if (result > 0) {
            // Calculer la polaire
            double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS];
            compute_polar(&grid, polar, &progress);

            gtk_widget_destroy(progress_dialog);

            // Charger directement en mémoire
            load_polar_from_grid(app->polar_data, &grid, polar);

            // Reconstruire l'interface
            rebuild_data_tab(app);
            rebuild_vmg_table(app);
            gtk_widget_queue_draw(app->polar_view);

            // Mettre à jour les combo box TWS
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_from_combo));
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_to_combo));

            for (int i = 0; i < app->polar_data->num_speeds; i++) {
                int tws = app->polar_data->tws_values[i];
                if (tws == 0) continue;
                char text[16];
                snprintf(text, sizeof(text), "%d kn", tws);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text);
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
            int last_idx = app->polar_data->num_speeds > 6 ? 6 : app->polar_data->num_speeds - 1;
            if (last_idx < 0) last_idx = 0;
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);

            gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, TR(app, "Polaire créée avec succès", "Polar created successfully"));
        } else {
            gtk_widget_destroy(progress_dialog);
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "Erreur lors du traitement des fichiers.\n\nAucune donnée valide trouvée.", "Error processing files.\n\nNo valid data found."));
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }

        free_polar_grid(&grid);
#ifdef _WIN32
        // Sur Windows, les fichiers sont alloués avec malloc(), pas g_malloc()
        g_slist_free_full(filenames, free);
#else
        g_slist_free_full(filenames, g_free);
#endif
    }
}

void on_update_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    if (app->polar_data->num_speeds == 0 || app->polar_data->num_angles == 0) {
        GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                          GTK_DIALOG_MODAL,
                                                          GTK_MESSAGE_ERROR,
                                                          GTK_BUTTONS_OK,
                                                          "%s", TR(app, "Aucune polaire chargée.\n\nUtilisez 'Créer' pour générer une nouvelle polaire.", "No polar loaded.\n\nUse 'Create' to generate a new polar."));
        gtk_dialog_run(GTK_DIALOG(error_dialog));
        gtk_widget_destroy(error_dialog);
        return;
    }

    GSList *filenames = NULL;

#ifdef _WIN32
    // Utiliser dialogue natif Windows
    filenames = win32_open_multi_dialog(app->window,
                                       TR(app, "Sélectionner fichier(s) NMEA ou VDR pour mise à jour", "Select NMEA or VDR file(s) to update"),
                                       TR(app, "Fichiers NMEA et VDR", "NMEA and VDR files"),
                                       "*.txt;*.nmea;*.log;*.db");
#else
    // Dialogue pour choisir le(s) fichier(s) de données
    GtkWidget *dialog = gtk_file_chooser_dialog_new(TR(app, "Sélectionner fichier(s) NMEA ou VDR pour mise à jour", "Select NMEA or VDR file(s) to update"),
                                                     GTK_WINDOW(app->window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                     TR(app, "_Ouvrir", "_Open"), GTK_RESPONSE_ACCEPT,
                                                     NULL);

    // Permettre la sélection multiple
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    // Filtres de fichiers - Filtre combiné NMEA et VDR par défaut
    GtkFileFilter *filter_data = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_data, TR(app, "Fichiers NMEA et VDR", "NMEA and VDR files"));
    gtk_file_filter_add_pattern(filter_data, "*.txt");
    gtk_file_filter_add_pattern(filter_data, "*.nmea");
    gtk_file_filter_add_pattern(filter_data, "*.log");
    gtk_file_filter_add_pattern(filter_data, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_data);

    GtkFileFilter *filter_nmea = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_nmea, TR(app, "Fichiers NMEA (*.txt, *.nmea, *.log)", "NMEA files (*.txt, *.nmea, *.log)"));
    gtk_file_filter_add_pattern(filter_nmea, "*.txt");
    gtk_file_filter_add_pattern(filter_nmea, "*.nmea");
    gtk_file_filter_add_pattern(filter_nmea, "*.log");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_nmea);

    GtkFileFilter *filter_vdr = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_vdr, TR(app, "Fichiers VDR (*.db)", "VDR files (*.db)"));
    gtk_file_filter_add_pattern(filter_vdr, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_vdr);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, TR(app, "Tous les fichiers", "All files"));
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    // Définir le filtre par défaut
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter_data);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);
#endif

    if (filenames) {

        // Créer le dialogue de progression
        GtkWidget *progress_dialog = gtk_dialog_new();
        gtk_window_set_title(GTK_WINDOW(progress_dialog), "Mise à jour de la polaire");
        gtk_window_set_transient_for(GTK_WINDOW(progress_dialog), GTK_WINDOW(app->window));
        gtk_window_set_modal(GTK_WINDOW(progress_dialog), TRUE);
        GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(progress_dialog));
        GtkWidget *progress_label = gtk_label_new("Initialisation...");
        gtk_box_pack_start(GTK_BOX(content_area), progress_label, TRUE, TRUE, 10);
        gtk_widget_show_all(progress_dialog);

        gboolean cancel_flag = FALSE;
        ProgressContext progress = {progress_dialog, progress_label, &cancel_flag};

        // Initialiser la grille polaire
        polar_grid_t grid;
        init_polar_grid(&grid);

        // Charger la polaire existante depuis la mémoire
        load_polar_from_memory(app->polar_data, &grid);

        // Calculer la polaire de référence
        compute_polar(&grid, grid.cached_polar, &progress);
        grid.cache_valid = true;

        // Traiter tous les nouveaux fichiers en mode update
        int total_result = 0;
        int file_count = 0;
        for (GSList *l = filenames; l != NULL; l = l->next) {
            char *filename = (char *)l->data;
            file_count++;

            char msg[256];
            snprintf(msg, sizeof(msg), "%s %d/%d...",
                     TR(app, "Traitement du fichier", "Processing file"),
                     file_count, g_slist_length(filenames));
            gtk_label_set_text(GTK_LABEL(progress_label), msg);
            while (gtk_events_pending()) gtk_main_iteration();

            int result = process_file(filename, &grid, true, &progress);
            if (result >= 0) total_result += result;
        }

        int result = total_result;

        if (result >= 0) {
            // Recalculer la polaire
            double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS];
            compute_polar(&grid, polar, &progress);

            gtk_widget_destroy(progress_dialog);

            // Mettre à jour les valeurs de la polaire existante (conserve la structure TWS/TWA)
            update_polar_from_grid(app->polar_data, polar);

            // Reconstruire l'interface
            rebuild_data_tab(app);
            rebuild_vmg_table(app);
            gtk_widget_queue_draw(app->polar_view);

            // Mettre à jour les combo box TWS
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_from_combo));
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_to_combo));

            for (int i = 0; i < app->polar_data->num_speeds; i++) {
                int tws = app->polar_data->tws_values[i];
                if (tws == 0) continue;
                char text[16];
                snprintf(text, sizeof(text), "%d kn", tws);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text);
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
            int last_idx = app->polar_data->num_speeds > 6 ? 6 : app->polar_data->num_speeds - 1;
            if (last_idx < 0) last_idx = 0;
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);

            gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, TR(app, "Polaire mise à jour avec succès", "Polar updated successfully"));
        } else {
            gtk_widget_destroy(progress_dialog);

            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "Erreur lors du traitement des fichiers.", "Error processing files."));
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }

        // Libérer la mémoire (grille et liste de fichiers)
        free_polar_grid(&grid);
#ifdef _WIN32
        g_slist_free_full(filenames, free);
#else
        g_slist_free_full(filenames, g_free);
#endif
    }
}

void on_add_twa_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Dialogue pour demander l'angle TWA
    GtkWidget *dialog = gtk_dialog_new_with_buttons(TR(app, "Ajouter un angle TWA", "Add TWA angle"),
                                                      GTK_WINDOW(app->window),
                                                      GTK_DIALOG_MODAL,
                                                      TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                      "_OK", GTK_RESPONSE_OK,
                                                      NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("TWA:"), FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 6);
    gtk_box_pack_start(GTK_BOX(hbox), entry, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(content), hbox);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        int new_twa = atoi(text);

        if (new_twa >= 0 && new_twa <= 180) {
            // Vérifier si l'angle existe déjà
            gboolean exists = FALSE;
            for (int i = 0; i < app->polar_data->num_angles; i++) {
                if (app->polar_data->twa_values[i] == new_twa) {
                    exists = TRUE;
                    break;
                }
            }

            if (exists) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s%d°%s",
                                                                  TR(app, "L'angle TWA ", "TWA angle "), new_twa, TR(app, " existe déjà.", " already exists."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
            } else if (app->polar_data->num_angles >= MAX_ANGLES) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s", TR(app, "Nombre maximum d'angles atteint.", "Maximum number of angles reached."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
            } else {
                // Trouver la position d'insertion (ordre croissant)
                int insert_pos = app->polar_data->num_angles;
                for (int i = 0; i < app->polar_data->num_angles; i++) {
                    if (app->polar_data->twa_values[i] > new_twa) {
                        insert_pos = i;
                        break;
                    }
                }

                // Décaler les angles suivants
                for (int i = app->polar_data->num_angles; i > insert_pos; i--) {
                    app->polar_data->twa_values[i] = app->polar_data->twa_values[i - 1];
                    app->polar_data->twa_present[i] = app->polar_data->twa_present[i - 1];
                    for (int j = 0; j < app->polar_data->num_speeds; j++) {
                        app->polar_data->polar_data[i][j] = app->polar_data->polar_data[i - 1][j];
                        strcpy(app->polar_data->polar_data_str[i][j], app->polar_data->polar_data_str[i - 1][j]);
                    }
                }

                // Insérer le nouvel angle
                app->polar_data->twa_values[insert_pos] = new_twa;
                app->polar_data->twa_present[insert_pos] = 1;
                for (int j = 0; j < app->polar_data->num_speeds; j++) {
                    app->polar_data->polar_data[insert_pos][j] = 0.0;
                    strcpy(app->polar_data->polar_data_str[insert_pos][j], "0.00");
                }
                app->polar_data->num_angles++;
                app->polar_data->modified = TRUE;

                // Reconstruire l'interface
                rebuild_data_tab(app);
                rebuild_vmg_table(app);
                gtk_widget_queue_draw(app->polar_view);
            }
        } else {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "L'angle TWA doit être entre 0 et 180°.", "TWA angle must be between 0 and 180°."));
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }
    }

    gtk_widget_destroy(dialog);
}

void on_add_tws_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Dialogue pour demander la vitesse TWS
    GtkWidget *dialog = gtk_dialog_new_with_buttons(TR(app, "Ajouter une vitesse TWS", "Add TWS speed"),
                                                      GTK_WINDOW(app->window),
                                                      GTK_DIALOG_MODAL,
                                                      TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                      "_OK", GTK_RESPONSE_OK,
                                                      NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);

    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("TWS:"), FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 6);
    gtk_box_pack_start(GTK_BOX(hbox), entry, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(content), hbox);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        int new_tws = atoi(text);

        if (new_tws > 0 && new_tws <= 100) {
            // Vérifier si la vitesse existe déjà
            gboolean exists = FALSE;
            for (int i = 0; i < app->polar_data->num_speeds; i++) {
                if (app->polar_data->tws_values[i] == new_tws) {
                    exists = TRUE;
                    break;
                }
            }

            if (exists) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s%d%s",
                                                                  TR(app, "La vitesse TWS ", "TWS speed "), new_tws, TR(app, " kn existe déjà.", " kn already exists."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
            } else if (app->polar_data->num_speeds >= MAX_SPEEDS) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s", TR(app, "Nombre maximum de vitesses atteint.", "Maximum number of speeds reached."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
            } else {
                // Trouver la position d'insertion (ordre croissant)
                int insert_pos = app->polar_data->num_speeds;
                for (int i = 0; i < app->polar_data->num_speeds; i++) {
                    if (app->polar_data->tws_values[i] > new_tws) {
                        insert_pos = i;
                        break;
                    }
                }

                // Décaler les vitesses suivantes
                for (int i = app->polar_data->num_speeds; i > insert_pos; i--) {
                    app->polar_data->tws_values[i] = app->polar_data->tws_values[i - 1];
                    for (int j = 0; j < app->polar_data->num_angles; j++) {
                        app->polar_data->polar_data[j][i] = app->polar_data->polar_data[j][i - 1];
                        strcpy(app->polar_data->polar_data_str[j][i], app->polar_data->polar_data_str[j][i - 1]);
                    }
                }

                // Insérer la nouvelle vitesse
                app->polar_data->tws_values[insert_pos] = new_tws;
                for (int j = 0; j < app->polar_data->num_angles; j++) {
                    app->polar_data->polar_data[j][insert_pos] = 0.0;
                    strcpy(app->polar_data->polar_data_str[j][insert_pos], "0.00");
                }
                app->polar_data->num_speeds++;
                app->polar_data->modified = TRUE;

                // Reconstruire l'interface
                rebuild_data_tab(app);
                rebuild_vmg_table(app);
                gtk_widget_queue_draw(app->polar_view);

                // Mettre à jour les combo box TWS
                gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_from_combo));
                gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_to_combo));

                for (int i = 0; i < app->polar_data->num_speeds; i++) {
                    int tws = app->polar_data->tws_values[i];
                    if (tws == 0) continue;
                    char text_buf[16];
                    snprintf(text_buf, sizeof(text_buf), "%d kn", tws);
                    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text_buf);
                    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text_buf);
                }
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
                int last_idx = app->polar_data->num_speeds > 6 ? 6 : app->polar_data->num_speeds - 1;
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);
            }
        } else {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "La vitesse TWS doit être entre 1 et 100 kn.", "TWS speed must be between 1 and 100 kn."));
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }
    }

    gtk_widget_destroy(dialog);
}

// Fonction de clignotement de la barre de statut
gboolean blink_status_bar(gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Obtenir le label de la barre de statut
    GtkWidget *label = gtk_statusbar_get_message_area(GTK_STATUSBAR(app->status_bar));
    if (label && GTK_IS_BOX(label)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(label));
        if (children && GTK_IS_LABEL(children->data)) {
            GtkWidget *status_label = GTK_WIDGET(children->data);

            if (app->blink_state) {
                char markup[256];
                snprintf(markup, sizeof(markup), "<b>%s</b>",
                         TR(app, "Mode suppression: cliquez sur un en-tête de ligne ou de colonne",
                                 "Delete mode: click on a row or column header"));
                gtk_label_set_markup(GTK_LABEL(status_label), markup);
            } else {
                gtk_label_set_text(GTK_LABEL(status_label), "");
            }
        }
        g_list_free(children);
    }

    app->blink_state = !app->blink_state;
    return TRUE;  // Continuer le timer
}

gboolean on_header_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    HeaderData *header_data = (HeaderData *)user_data;
    AppWidgets *app = header_data->app;

    if (!app->delete_mode) return FALSE;

    // Arrêter le clignotement et restaurer la barre de statut
    if (app->blink_timer_id > 0) {
        g_source_remove(app->blink_timer_id);
        app->blink_timer_id = 0;
    }
    app->delete_mode = FALSE;

    // Demander confirmation
    char message[256];
    if (header_data->is_twa) {
        int twa = app->polar_data->twa_values[header_data->index];
        snprintf(message, sizeof(message), "%s%d°%s",
                 TR(app, "Voulez-vous supprimer la ligne TWA ", "Do you want to delete TWA row "),
                 twa,
                 TR(app, " ?", "?"));
    } else {
        int tws = app->polar_data->tws_values[header_data->index];
        snprintf(message, sizeof(message), "%s%d%s",
                 TR(app, "Voulez-vous supprimer la colonne TWS ", "Do you want to delete TWS column "),
                 tws,
                 TR(app, " kn ?", " kn?"));
    }

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                GTK_DIALOG_MODAL,
                                                GTK_MESSAGE_QUESTION,
                                                GTK_BUTTONS_YES_NO,
                                                "%s", message);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        if (header_data->is_twa) {
            // Supprimer la ligne TWA
            if (app->polar_data->num_angles <= 1) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s", TR(app, "Impossible de supprimer la dernière ligne.", "Cannot delete the last row."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                return TRUE;
            }

            // Décaler les lignes suivantes
            for (int i = header_data->index; i < app->polar_data->num_angles - 1; i++) {
                app->polar_data->twa_values[i] = app->polar_data->twa_values[i + 1];
                app->polar_data->twa_present[i] = app->polar_data->twa_present[i + 1];
                for (int j = 0; j < app->polar_data->num_speeds; j++) {
                    app->polar_data->polar_data[i][j] = app->polar_data->polar_data[i + 1][j];
                    strcpy(app->polar_data->polar_data_str[i][j], app->polar_data->polar_data_str[i + 1][j]);
                }
            }
            app->polar_data->num_angles--;
        } else {
            // Supprimer la colonne TWS
            if (app->polar_data->num_speeds <= 1) {
                GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                  GTK_DIALOG_MODAL,
                                                                  GTK_MESSAGE_ERROR,
                                                                  GTK_BUTTONS_OK,
                                                                  "%s", TR(app, "Impossible de supprimer la dernière colonne.", "Cannot delete the last column."));
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
                return TRUE;
            }

            // Décaler les colonnes suivantes
            for (int i = header_data->index; i < app->polar_data->num_speeds - 1; i++) {
                app->polar_data->tws_values[i] = app->polar_data->tws_values[i + 1];
                for (int j = 0; j < app->polar_data->num_angles; j++) {
                    app->polar_data->polar_data[j][i] = app->polar_data->polar_data[j][i + 1];
                    strcpy(app->polar_data->polar_data_str[j][i], app->polar_data->polar_data_str[j][i + 1]);
                }
            }
            app->polar_data->num_speeds--;

            // Mettre à jour les combo box TWS
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_from_combo));
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->tws_to_combo));

            for (int i = 0; i < app->polar_data->num_speeds; i++) {
                int tws = app->polar_data->tws_values[i];
                if (tws == 0) continue;
                char text[16];
                snprintf(text, sizeof(text), "%d kn", tws);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_from_combo), text);
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->tws_to_combo), text);
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_from_combo), 0);
            int last_idx = app->polar_data->num_speeds > 6 ? 6 : app->polar_data->num_speeds - 1;
            gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);
        }

        app->polar_data->modified = TRUE;

        // Reconstruire l'interface
        rebuild_data_tab(app);
        rebuild_vmg_table(app);
        gtk_widget_queue_draw(app->polar_view);

        gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, TR(app, "Prêt", "Ready"));
    } else {
        // L'utilisateur a cliqué sur "Non", sortir du mode suppression
        gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, TR(app, "Prêt", "Ready"));
    }

    return TRUE;
}

void on_delete_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Activer le mode suppression
    app->delete_mode = TRUE;
    app->blink_state = TRUE;

    // Démarrer le clignotement (500ms)
    if (app->blink_timer_id > 0) {
        g_source_remove(app->blink_timer_id);
    }
    app->blink_timer_id = g_timeout_add(500, blink_status_bar, app);
}

void on_quit_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (prompt_save_changes(app)) {
        gtk_main_quit();
    }
}

gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (prompt_save_changes(app)) {
        return FALSE;  // Permettre la fermeture
    }
    return TRUE;  // Empêcher la fermeture
}

// Créer la fenêtre principale
void create_main_window(AppWidgets *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Polar Doctor");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1200, 700);

    // Définir l'icône de la fenêtre
    GError *error = NULL;
    if (gtk_window_set_icon_from_file(GTK_WINDOW(app->window), "polar_doctor.png", &error)) {
        // Icône chargée avec succès
    } else if (error) {
        g_warning("Impossible de charger l'icône: %s", error->message);
        g_error_free(error);
    }

    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_window_delete), app);
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    // Barre de boutons
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 5);

    app->btn_open = gtk_button_new_with_label(TR(app, "Ouvrir", "Open"));
    app->btn_save = gtk_button_new_with_label(TR(app, "Enregistrer", "Save"));
    app->btn_create = gtk_button_new_with_label(TR(app, "Créer", "Create"));
    app->btn_update = gtk_button_new_with_label(TR(app, "Mettre à jour", "Update"));
    app->btn_print = gtk_button_new_with_label(TR(app, "Imprimer", "Print"));

    g_signal_connect(app->btn_open, "clicked", G_CALLBACK(on_open_clicked), app);
    g_signal_connect(app->btn_save, "clicked", G_CALLBACK(on_save_clicked), app);
    g_signal_connect(app->btn_create, "clicked", G_CALLBACK(on_create_clicked), app);
    g_signal_connect(app->btn_update, "clicked", G_CALLBACK(on_update_clicked), app);
    g_signal_connect(app->btn_print, "clicked", G_CALLBACK(on_print_clicked), app);

    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_create, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_update, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_print, FALSE, FALSE, 0);

    // Séparateur
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator, FALSE, FALSE, 5);

    // Boutons d'édition
    app->btn_add_twa = gtk_button_new_with_label(TR(app, "Ajout TWA", "Add TWA"));
    app->btn_add_tws = gtk_button_new_with_label(TR(app, "Ajout TWS", "Add TWS"));
    app->btn_delete = gtk_button_new_with_label(TR(app, "Suppression", "Delete"));

    g_signal_connect(app->btn_add_twa, "clicked", G_CALLBACK(on_add_twa_clicked), app);
    g_signal_connect(app->btn_add_tws, "clicked", G_CALLBACK(on_add_tws_clicked), app);
    g_signal_connect(app->btn_delete, "clicked", G_CALLBACK(on_delete_clicked), app);

    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_add_twa, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_add_tws, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_delete, FALSE, FALSE, 0);

    // Séparateur
    GtkWidget *separator2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator2, FALSE, FALSE, 5);

    // Bouton Aide
    app->btn_help = gtk_button_new_with_label(TR(app, "Aide", "Help"));
    g_signal_connect(app->btn_help, "clicked", G_CALLBACK(on_help_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_help, FALSE, FALSE, 0);

    // Bouton de langue avec drapeau français (par défaut) - tout à droite
    app->lang_button = gtk_button_new_with_label("🇫🇷");
    g_signal_connect(app->lang_button, "clicked", G_CALLBACK(on_lang_clicked), app);
    gtk_box_pack_end(GTK_BOX(toolbar), app->lang_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    // Notebook avec onglets
    app->notebook = gtk_notebook_new();

    GtkWidget *data_tab = create_data_tab(app);
    GtkWidget *diagram_tab = create_diagram_tab(app);

    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), data_tab, gtk_label_new("Données de la polaire"));
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), diagram_tab, gtk_label_new("Diagramme de la polaire"));

    gtk_box_pack_start(GTK_BOX(vbox), app->notebook, TRUE, TRUE, 0);

    // Barre de status
    app->status_bar = gtk_statusbar_new();
    gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, "Prêt");
    gtk_box_pack_start(GTK_BOX(vbox), app->status_bar, FALSE, FALSE, 0);

    // Initialiser la langue par défaut
    app->language = LANG_FR;
}

void on_lang_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Basculer la langue
    if (app->language == LANG_FR) {
        app->language = LANG_EN;
        gtk_button_set_label(GTK_BUTTON(app->lang_button), "🇬🇧");
    } else {
        app->language = LANG_FR;
        gtk_button_set_label(GTK_BUTTON(app->lang_button), "🇫🇷");
    }

    // Mettre à jour l'interface
    update_interface_language(app);
}

void update_interface_language(AppWidgets *app) {
    // Mettre à jour le titre de la fenêtre
    gtk_window_set_title(GTK_WINDOW(app->window),
                         TR(app, "Polar Doctor", "Polar Doctor"));

    // Mettre à jour les titres des onglets
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(app->notebook),
                                     gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), 0),
                                     TR(app, "Données", "Data"));
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(app->notebook),
                                     gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), 1),
                                     TR(app, "Polaire", "Polar"));
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(app->notebook),
                                     gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), 2),
                                     "VMG");

    // Mettre à jour les labels des boutons
    gtk_button_set_label(GTK_BUTTON(app->btn_open), TR(app, "Ouvrir", "Open"));
    gtk_button_set_label(GTK_BUTTON(app->btn_save), TR(app, "Enregistrer", "Save"));
    gtk_button_set_label(GTK_BUTTON(app->btn_create), TR(app, "Créer", "Create"));
    gtk_button_set_label(GTK_BUTTON(app->btn_update), TR(app, "Mettre à jour", "Update"));
    gtk_button_set_label(GTK_BUTTON(app->btn_print), TR(app, "Imprimer", "Print"));
    gtk_button_set_label(GTK_BUTTON(app->btn_add_twa), TR(app, "Ajout TWA", "Add TWA"));
    gtk_button_set_label(GTK_BUTTON(app->btn_add_tws), TR(app, "Ajout TWS", "Add TWS"));
    gtk_button_set_label(GTK_BUTTON(app->btn_delete), TR(app, "Suppression", "Delete"));
    gtk_button_set_label(GTK_BUTTON(app->btn_help), TR(app, "Aide", "Help"));

    // Mettre à jour les labels TWS
    gtk_label_set_text(GTK_LABEL(app->tws_from_label), TR(app, "Voir les polaires pour les TWS de", "View polars for TWS from"));
    gtk_label_set_text(GTK_LABEL(app->tws_to_label), TR(app, "à", "to"));

    // Mettre à jour la barre de statut
    gtk_statusbar_pop(GTK_STATUSBAR(app->status_bar), 0);
    gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0,
                       TR(app, "Prêt", "Ready"));

    // Reconstruire le tableau de données et VMG pour mettre à jour les en-têtes
    rebuild_data_tab(app);
    rebuild_vmg_table(app);
}

void on_help_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    const char *help_text_fr =
        "<b>Polar Doctor - Mode d'emploi</b>\n\n"
        "<b>BOUTONS PRINCIPAUX</b>\n\n"
        "<b>• Ouvrir</b>\n"
        "  Charge un fichier polaire existant (.pol)\n"
        "  Si des modifications non sauvegardées existent, un dialogue de confirmation s'affiche.\n\n"
        "<b>• Enregistrer</b>\n"
        "  Sauvegarde la polaire actuelle dans un fichier .pol\n"
        "  L'extension .pol est ajoutée automatiquement si non spécifiée.\n\n"
        "<b>• Créer</b>\n"
        "  Génère une nouvelle polaire à partir de fichiers NMEA ou VDR\n"
        "  - Sélection multiple possible (Ctrl+clic ou Shift+clic)\n"
        "  - Fichiers supportés : .txt, .nmea, .log (NMEA) et .db (VDR)\n"
        "  - Tous les fichiers sont traités et combinés dans une seule polaire\n"
        "  - Affiche la progression pendant le traitement\n\n"
        "<b>• Mettre à jour</b>\n"
        "  Met à jour la polaire actuelle avec de nouvelles données NMEA/VDR\n"
        "  - Sélection multiple possible\n"
        "  - Seules les performances égales ou supérieures sont conservées\n"
        "  - Les données inférieures à 95% de l'existant sont filtrées\n\n"
        "<b>• Imprimer</b>\n"
        "  Imprime la polaire (tableau de données + diagramme + VMG) sur papier ou PDF\n\n"
        "<b>ÉDITION DES DONNÉES</b>\n\n"
        "<b>• Ajout TWA</b>\n"
        "  Insère une nouvelle ligne d'angle de vent (TWA)\n"
        "  - Saisir l'angle entre 0° et 180°\n"
        "  - La ligne est insérée dans l'ordre croissant\n"
        "  - Toutes les cellules sont initialisées à 0.00\n\n"
        "<b>• Ajout TWS</b>\n"
        "  Insère une nouvelle colonne de vitesse de vent (TWS)\n"
        "  - Saisir la vitesse en nœuds\n"
        "  - La colonne est insérée dans l'ordre croissant\n"
        "  - Toutes les cellules sont initialisées à 0.00\n\n"
        "<b>• Suppression</b>\n"
        "  Supprime une ligne ou une colonne\n"
        "  - Cliquer sur ce bouton, puis cliquer sur l'en-tête à supprimer\n"
        "  - Un message clignotant en gras apparaît dans la barre d'état\n"
        "  - Une confirmation est demandée avant suppression\n\n"
        "<b>ONGLETS</b>\n\n"
        "<b>• Données</b>\n"
        "  Tableau éditable des vitesses (BSP) en fonction de TWA et TWS\n"
        "  - Double-cliquer sur une cellule pour modifier la valeur\n"
        "  - Cliquer sur les en-têtes en mode suppression pour supprimer ligne/colonne\n\n"
        "<b>• Polaire</b>\n"
        "  Visualisation graphique du diagramme polaire\n"
        "  - Sélectionner la plage de TWS à afficher (de/à)\n"
        "  - Les courbes sont lissées avec interpolation Catmull-Rom\n"
        "  - TWS 0 est exclu de l'affichage\n\n"
        "<b>• VMG</b>\n"
        "  Tableau des meilleures performances VMG (Velocity Made Good)\n"
        "  - VMG up : meilleur angle au près\n"
        "  - VMG down : meilleur angle au portant\n"
        "  - Calculé pour chaque vitesse de vent (TWS)\n\n"
        "<b>FORMAT DES FICHIERS</b>\n\n"
        "<b>• Fichiers NMEA (.txt, .nmea, .log)</b>\n"
        "  Nécessite les sentences MWV (vent) et VHW (vitesse bateau)\n"
        "  Les données sont groupées par buckets de 5° (TWA) et 2 kn (TWS)\n\n"
        "<b>• Fichiers VDR (.db)</b>\n"
        "  Base SQLite générée par qtVlm\n"
        "  Requête : SELECT TWA, TWS, STW FROM VDR\n\n"
        "<b>• Fichiers polaires (.pol)</b>\n"
        "  Format tabulaire avec séparateur point-virgule\n"
        "  Première ligne : en-têtes TWS\n"
        "  Colonnes suivantes : TWA + vitesses BSP\n\n"
        "<b>CONSEILS</b>\n\n"
        "• Combiner plusieurs sorties de navigation pour plus de précision\n"
        "• Utiliser 'Mettre à jour' pour améliorer progressivement une polaire\n"
        "• Les valeurs sont toujours affichées avec 2 décimales\n"
        "• Un minimum de 3 points est requis par bucket pour calculer une moyenne\n"
        "• Les valeurs extrêmes sont éliminées (trimmed mean) si ≥5 points";

    const char *help_text_en =
        "<b>Polar Doctor - User Manual</b>\n\n"
        "<b>MAIN BUTTONS</b>\n\n"
        "<b>• Open</b>\n"
        "  Load an existing polar file (.pol)\n"
        "  If unsaved changes exist, a confirmation dialog appears.\n\n"
        "<b>• Save</b>\n"
        "  Save the current polar to a .pol file\n"
        "  The .pol extension is automatically added if not specified.\n\n"
        "<b>• Create</b>\n"
        "  Generate a new polar from NMEA or VDR files\n"
        "  - Multiple selection possible (Ctrl+click or Shift+click)\n"
        "  - Supported files: .txt, .nmea, .log (NMEA) and .db (VDR)\n"
        "  - All files are processed and combined into a single polar\n"
        "  - Progress is displayed during processing\n\n"
        "<b>• Update</b>\n"
        "  Update the current polar with new NMEA/VDR data\n"
        "  - Multiple selection possible\n"
        "  - Only equal or better performances are kept\n"
        "  - Data below 95% of existing values is filtered\n\n"
        "<b>• Print</b>\n"
        "  Print the polar (data table + diagram + VMG) to paper or PDF\n\n"
        "<b>DATA EDITING</b>\n\n"
        "<b>• Add TWA</b>\n"
        "  Insert a new wind angle row (TWA)\n"
        "  - Enter angle between 0° and 180°\n"
        "  - Row is inserted in ascending order\n"
        "  - All cells are initialized to 0.00\n\n"
        "<b>• Add TWS</b>\n"
        "  Insert a new wind speed column (TWS)\n"
        "  - Enter speed in knots\n"
        "  - Column is inserted in ascending order\n"
        "  - All cells are initialized to 0.00\n\n"
        "<b>• Delete</b>\n"
        "  Delete a row or column\n"
        "  - Click this button, then click the header to delete\n"
        "  - A blinking bold message appears in the status bar\n"
        "  - Confirmation is requested before deletion\n\n"
        "<b>TABS</b>\n\n"
        "<b>• Data</b>\n"
        "  Editable table of speeds (BSP) by TWA and TWS\n"
        "  - Double-click a cell to edit the value\n"
        "  - Click headers in delete mode to remove row/column\n\n"
        "<b>• Polar</b>\n"
        "  Graphical polar diagram visualization\n"
        "  - Select the TWS range to display (from/to)\n"
        "  - Curves are smoothed with Catmull-Rom interpolation\n"
        "  - TWS 0 is excluded from display\n\n"
        "<b>• VMG</b>\n"
        "  Table of best VMG (Velocity Made Good) performances\n"
        "  - VMG up: best upwind angle\n"
        "  - VMG down: best downwind angle\n"
        "  - Calculated for each wind speed (TWS)\n\n"
        "<b>FILE FORMATS</b>\n\n"
        "<b>• NMEA files (.txt, .nmea, .log)</b>\n"
        "  Requires MWV (wind) and VHW (boat speed) sentences\n"
        "  Data is grouped by 5° (TWA) and 2 kn (TWS) buckets\n\n"
        "<b>• VDR files (.db)</b>\n"
        "  SQLite database generated by qtVlm\n"
        "  Query: SELECT TWA, TWS, STW FROM VDR\n\n"
        "<b>• Polar files (.pol)</b>\n"
        "  Tabular format with semicolon separator\n"
        "  First row: TWS headers\n"
        "  Following columns: TWA + BSP speeds\n\n"
        "<b>TIPS</b>\n\n"
        "• Combine multiple sailing sessions for better accuracy\n"
        "• Use 'Update' to progressively improve a polar\n"
        "• Values are always displayed with 2 decimals\n"
        "• Minimum of 3 points required per bucket to calculate average\n"
        "• Extreme values are eliminated (trimmed mean) if ≥5 points";

    const char *help_text = TR(app, help_text_fr, help_text_en);

    // Créer un dialogue personnalisé avec ascenseur
    GtkWidget *dialog = gtk_dialog_new_with_buttons(TR(app, "Aide - Polar Doctor", "Help - Polar Doctor"),
                                                      GTK_WINDOW(app->window),
                                                      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                      "_OK", GTK_RESPONSE_OK,
                                                      NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);

    // Zone de contenu
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    // Créer une fenêtre scrollable
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled_window, TRUE);

    // Label avec le texte d'aide
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), help_text);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 80);
    gtk_widget_set_margin_start(label, 10);
    gtk_widget_set_margin_end(label, 10);
    gtk_widget_set_margin_top(label, 10);
    gtk_widget_set_margin_bottom(label, 10);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);

    // Ajouter le label dans la fenêtre scrollable
    gtk_container_add(GTK_CONTAINER(scrolled_window), label);
    gtk_box_pack_start(GTK_BOX(content_area), scrolled_window, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

int main(int argc, char *argv[]) {
    // Fix pour Windows - désactive les portails GTK qui peuvent causer des crashes
    #ifdef _WIN32
    g_setenv("GTK_USE_PORTAL", "0", TRUE);
    g_setenv("GDK_BACKEND", "win32", TRUE);
    // Désactive le backend natif de fichiers qui peut causer des problèmes
    g_setenv("GTK_FILE_CHOOSER_BACKEND", "gtk", TRUE);
    #endif

    gtk_init(&argc, &argv);

    // Forcer la locale C APRÈS gtk_init pour que atof() utilise le point comme séparateur décimal
    // GTK réinitialise la locale, donc on doit la forcer après
    setlocale(LC_NUMERIC, "C");

    PolarData polar_data;
    init_polar_data(&polar_data);

    AppWidgets app;
    app.polar_data = &polar_data;
    app.delete_mode = FALSE;
    app.blink_timer_id = 0;
    app.blink_state = FALSE;

    create_main_window(&app);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
