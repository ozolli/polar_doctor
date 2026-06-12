/*
 * polar_doctor.h
 * Déclarations partagées : types, constantes, globals, prototypes.
 * Le code est découpé en modules (.c) compilés ensemble ; ce header est le
 * point de déclaration unique commun à tous.
 */
#ifndef POLAR_DOCTOR_H
#define POLAR_DOCTOR_H

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
#include <limits.h>
#include <glib/gstdio.h>

// Configuration spécifique Windows pour les dialogues de fichiers
#ifdef _WIN32
#include <windows.h>
#define GTK_FILE_CHOOSER_NATIVE_DISABLED 1
#define WINDOWS_PRINT_SCALE 1.0
#else
#ifndef MAX_PATH
#ifdef PATH_MAX
#define MAX_PATH PATH_MAX
#else
#define MAX_PATH 4096
#endif
#endif
#define WINDOWS_PRINT_SCALE 1.0
#endif

#define MAX_ANGLES 37    // 0° à 180° par pas de 5°
#define MAX_SPEEDS 16    // 4 à 70 kn par pas de variable
#define ANGLE_STEP 5
#define MIN_TWS 4
#define MAX_TWS 70

// Constantes pour polar_generator
#define PG_MAX_LINE 512
#define PG_MAX_SPEEDS 100
#define PG_MAX_ANGLES 181
#define PG_ANGLE_STEP 5
#define PG_SPEED_STEP 2

// Agrégation : percentile de la BSP retenu par cellule (90 = P90 par défaut).
#define DEFAULT_POLAR_PERCENTILE 90
extern int g_polar_percentile;

// Moteur : RPM > 0 = moteur en route -> exclu, SAUF commentaire de charge (débrayé).
// Les mots-clés Moteur/Charge sont configurables par bateau (g_boat_config).

// Lissage glissant à l'import NMEA (moyenne mobile sur N points complets).
#define NMEA_SMOOTH_WINDOW 10
#define NMEA_SMOOTH_BUFSZ (NMEA_SMOOTH_WINDOW > 1 ? NMEA_SMOOTH_WINDOW : 1)
#define NMEA_SMOOTH_TWA_BREAK 40.0

// Débruitage du loch (STW) par comparaison à la vitesse fond (SOG).
#define STW_SOG_TOL 1.5
#define STW_SOG_ALPHA 0.1
#define STW_SOG_GAP_RESET 1800

// ---- Configuration du bateau (inventaire voiles / états de mer + mots-clés moteur) ----
#define BOAT_MAX_ITEMS 32
#define BOAT_TERM_LEN  48
#define BOAT_MAX_POLARS 16

// Une polaire du bateau = un nom + des critères de routage par dimension.
// Liste vide sur une dimension = « tout » (pas de contrainte). Un point est routé
// vers la polaire si sa GV ∈ mains (ou vide) ET sa voile d'avant ∈ heads (ou vide)
// ET son état de mer ∈ seas (ou vide).
typedef struct {
    char name[64];
    char mains[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_mains;  // états de GV concernés
    char heads[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_heads;  // voiles d'avant concernées
    char seas[BOAT_MAX_ITEMS][BOAT_TERM_LEN];  int n_seas;   // états de mer concernés
} PolarDef;

typedef struct {
    char name[128];
    char mainsail[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_mainsail;  // états de grand-voile
    char headsail[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_headsail;  // voiles d'avant
    char seastate[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_seastate;  // états de mer
    char kw_moteur[BOAT_TERM_LEN];   // moteur embrayé -> exclure
    char kw_charge[BOAT_TERM_LEN];   // charge batteries débrayé -> garder
    PolarDef polars[BOAT_MAX_POLARS]; int n_polars;  // polaires définies pour ce bateau
} BoatConfig;

extern BoatConfig g_boat_config;
extern char g_boat_config_path[];   // chemin du config du bateau ouvert ("" si aucun)

// Données NMEA en cours d'assemblage (paire MWV+VHW, + SOG le plus récent).
typedef struct {
    double tws, twa, bsp;
    bool has_tws, has_twa, has_bsp;
    double sog;
    bool has_sog;
    double twd;       // direction vraie du vent (MWD) ; TWA = TWD - cap
    bool has_twd;
    double heading;   // cap vrai (HDT/HDG/VHW)
    bool has_heading;
} nmea_data_t;

// Filtre de débruitage STW (offset courant STW-SOG suivi par EMA).
typedef struct {
    double offset;
    bool have_offset;
} stw_sog_filter_t;

// Moyenne mobile circulaire des derniers points NMEA complets (voir NMEA_SMOOTH_WINDOW).
typedef struct {
    double twa[NMEA_SMOOTH_BUFSZ];
    double tws[NMEA_SMOOTH_BUFSZ];
    double bsp[NMEA_SMOOTH_BUFSZ];
    int count;
    int head;
    double last_twa;
    bool have_last;
} nmea_smoother_t;
void nmea_smoother_reset(nmea_smoother_t *s);
void nmea_smoother_push(nmea_smoother_t *s, double twa, double tws, double bsp,
                        double *out_twa, double *out_tws, double *out_bsp);

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

// Routage multi-polaires à l'import : chaque point est rangé dans toutes les grilles
// dont la définition matche l'état courant (GV/voile d'avant/mer). Si g_polar_router
// est NULL, l'import alimente une grille unique (comportement classique).
typedef struct {
    polar_grid_t *grids;     // n grilles (une par polaire)
    const PolarDef *defs;    // n définitions (g_boat_config.polars)
    int n;
} polar_router_t;

extern polar_router_t *g_polar_router;

// Visualisation live : grille dont on superpose les points bruts sur le diagramme
// (NULL hors capture), + point courant (bsp <= 0 = aucun).
extern polar_grid_t *g_live_grid;
extern double g_live_cur_twa, g_live_cur_bsp;

typedef struct {
    GtkWidget *dialog;
    GtkWidget *label;
    gboolean *cancel_flag;
} ProgressContext;

typedef struct {
    double polar_data[MAX_ANGLES][MAX_SPEEDS];
    char polar_data_str[MAX_ANGLES][MAX_SPEEDS][16];
    int tws_values[MAX_SPEEDS];
    int num_speeds;
    int twa_values[MAX_ANGLES];
    int twa_present[MAX_ANGLES];
    int num_angles;
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
    GtkWidget *vmg_container;
    GtkWidget *lang_button;
    GtkWidget *btn_open;
    GtkWidget *btn_new_boat;
    GtkWidget *boat_name_label;  // nom du bateau ouvert (panneau latéral)
    GtkWidget *polar_list;       // liste des polaires du bateau (panneau latéral)
    GtkWidget *btn_save;
    GtkWidget *btn_create;
    GtkWidget *btn_update;
    GtkWidget *btn_export_pdf;
    GtkWidget *btn_add_twa;
    GtkWidget *btn_add_tws;
    GtkWidget *btn_delete;
    GtkWidget *btn_boat;
    GtkWidget *btn_help;
    GtkWidget *percentile_spin;
    GtkWidget *percentile_label;
    GtkWidget *dynamic_check;
    GtkWidget *dynamic_tws_spin;
    GtkWidget *dynamic_tws_label;
    GtkWidget *dynamic_info;
    GtkWidget *legend_container;
    gboolean dynamic_mode;
    gboolean dragging;
    double drag_twa;
    GtkWidget *tws_from_label;
    GtkWidget *tws_to_label;
    PolarData *polar_data;
    gboolean delete_mode;
    guint blink_timer_id;
    gboolean blink_state;
    Language language;
} AppWidgets;

typedef struct {
    AppWidgets *app;
    int angle_idx;
    int speed_idx;
} CellData;

typedef struct {
    AppWidgets *app;
    int index;
    gboolean is_twa;
} HeaderData;

typedef struct {
    const char *fr;
    const char *en;
} Translation;

#define TR(app, str_fr, str_en) ((app)->language == LANG_FR ? (str_fr) : (str_en))

// ---- Prototypes ----

// boat_config.c
#define BOAT_PATH_LEN 512   // longueur max d'un chemin de bateau/config
#define BOAT_RECENT_MAX 8   // nb de bateaux récents conservés
void boat_config_init(BoatConfig *c);
char *boat_str_trim(char *s);
void boat_list_add(char list[][BOAT_TERM_LEN], int *n, const char *term);
bool boat_config_load(BoatConfig *c, const char *path);
bool boat_config_save(const BoatConfig *c, const char *path);
const char *sea_state_label(const char *fr, Language lang);  // affichage FR/EN (Douglas)
// Un dossier est « un bateau » s'il contient un config : boat.cfg, sinon <nom>.cfg,
// sinon le premier *.cfg. Renvoie true + chemin complet du config dans out.
bool boat_find_config(const char *folder, char *out, size_t outsz);
// Liste des bateaux récents (chemins de dossiers), le plus récent en tête.
int boat_recent_load(char list[][BOAT_PATH_LEN], int max);
void boat_recent_add(const char *boat_folder);
// Une polaire matche-t-elle l'état courant (GV / voile d'avant / état de mer) ?
bool polar_def_matches(const PolarDef *pd, const char *cur_main,
                       const char *cur_head, const char *cur_sea);

// import.c
void stw_sog_reset(stw_sog_filter_t *f);
bool stw_sog_accept(stw_sog_filter_t *f, double stw, double sog);
void init_polar_grid(polar_grid_t *grid);
void free_polar_grid(polar_grid_t *grid);
bool verify_checksum(const char *sentence);
int round_to_bucket(double value, int step);
int nmea_split(char *s, char **out, int maxf);
bool nmea_field_num(char **f, int nf, int idx, double *out);
void parse_sog_sentence(const char *type, char **f, int nf, nmea_data_t *data);
bool parse_nmea_sentence(const char *sentence, nmea_data_t *data);
void add_data_point(polar_grid_t *grid, double twa, double tws, double bsp);
double aggregate_cell(data_point_t *head);
double get_polar_value(polar_grid_t *grid, int angle, int speed);
int process_nmea_file(const char *filename, polar_grid_t *grid, ProgressContext *progress);
bool vdr_has_column(sqlite3 *db, const char *col);
bool comment_has_keyword(const char *comment, const char *keyword);
int process_vdr_file(const char *filename, polar_grid_t *grid, ProgressContext *progress);
bool is_vdr_file(const char *filename);
int process_file(const char *filename, polar_grid_t *grid, ProgressContext *progress);
bool load_existing_polar_for_update(const char *filename, polar_grid_t *grid, ProgressContext *progress);
void compute_polar(polar_grid_t *grid, double result[PG_MAX_ANGLES][PG_MAX_SPEEDS], ProgressContext *progress);

// polar_data.c
void load_polar_from_memory(PolarData *data, polar_grid_t *grid);
double interpolate_bsp(double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS], double twa, double tws);
void update_polar_from_grid(PolarData *data, double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS]);
void load_polar_from_grid(PolarData *data, polar_grid_t *grid, double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS]);
int combo_index_to_tws_index(PolarData *data, int combo_idx);
int tws_default_to_index(PolarData *data);
void init_polar_data(PolarData *data);
gboolean load_polar_file(const char *filename, PolarData *data);
gboolean save_polar_file(const char *filename, PolarData *data);
double interpolate_polar_bsp(PolarData *data, double twa, double tws);
double dynamic_curve_max(PolarData *data, double tws, double *out_twa);
double polar_absolute_max(PolarData *data, double *out_tws, double *out_twa);
void tws_palette_color(int idx, double *r, double *g, double *b);
void vmg_optimal_angles(PolarData *data, double tws, double *a_up, double *a_dn);

// diagram.c
gboolean draw_polar_diagram(GtkWidget *widget, cairo_t *cr, gpointer user_data);
double diagram_event_twa(GtkWidget *w, double ex, double ey);
void update_dynamic_info(AppWidgets *app);
gboolean on_diagram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean on_diagram_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean on_diagram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_dynamic_toggled(GtkWidget *widget, gpointer user_data);
void on_dynamic_tws_changed(GtkWidget *widget, gpointer user_data);

// gui_tabs.c
void rebuild_data_tab(AppWidgets *app);
void update_data_tab(AppWidgets *app);
GtkWidget *create_data_tab(AppWidgets *app);
GtkWidget *create_diagram_tab(AppWidgets *app);
void rebuild_vmg_table(AppWidgets *app);
void rebuild_legend(AppWidgets *app);
void on_cell_changed(GtkEntry *entry, gpointer user_data);
void on_tws_changed(GtkWidget *widget, gpointer user_data);
gboolean prompt_save_changes(AppWidgets *app);
void on_open_clicked(GtkWidget *widget, gpointer user_data);
void on_save_clicked(GtkWidget *widget, gpointer user_data);
void refresh_after_polar_load(AppWidgets *app, const char *filename);

// export_pdf.c
void on_export_pdf_clicked(GtkWidget *widget, gpointer user_data);
void print_begin(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data);
void print_page(GtkPrintOperation *operation, GtkPrintContext *context, gint page_nr, gpointer user_data);

// gui_window.c
void on_create_clicked(GtkWidget *widget, gpointer user_data);
void on_update_clicked(GtkWidget *widget, gpointer user_data);
void on_add_twa_clicked(GtkWidget *widget, gpointer user_data);
void on_add_tws_clicked(GtkWidget *widget, gpointer user_data);
gboolean blink_status_bar(gpointer user_data);
gboolean on_header_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_delete_clicked(GtkWidget *widget, gpointer user_data);
void on_quit_clicked(GtkWidget *widget, gpointer user_data);
gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data);
void create_main_window(AppWidgets *app);
void on_lang_clicked(GtkWidget *widget, gpointer user_data);
void on_percentile_changed(GtkWidget *widget, gpointer user_data);
void update_interface_language(AppWidgets *app);
void on_help_clicked(GtkWidget *widget, gpointer user_data);
void on_boat_config_clicked(GtkWidget *widget, gpointer user_data);
// Gestion bateau = dossier (config + polaire(s))
void open_boat(AppWidgets *app, const char *folder);
void on_open_menu(GtkWidget *widget, gpointer user_data);
void on_recent_boat_activate(GtkWidget *widget, gpointer user_data);
void on_browse_boat(GtkWidget *widget, gpointer user_data);
void on_new_boat_clicked(GtkWidget *widget, gpointer user_data);

// live.c — capture live (tail VDR qtVlm + annotations par boutons + routage polaires)
GtkWidget *create_live_tab(AppWidgets *app);
void live_inventory_changed(void);

// win32_dialogs.c (Windows uniquement)
#ifdef _WIN32
wchar_t *utf8_to_wchar(const char *utf8);
char *wchar_to_utf8(const wchar_t *wstr);
char *win32_save_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                        const char *filter_pattern, const char *default_name);
char *win32_open_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                        const char *filter_pattern);
GSList *win32_open_multi_dialog(GtkWidget *parent, const char *title, const char *filter_name,
                                const char *filter_pattern);
#endif

#endif // POLAR_DOCTOR_H
