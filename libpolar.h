/*
 * libpolar.h
 * Cœur métier de polar_doctor, INDÉPENDANT de l'UI : types, constantes, globals
 * et prototypes du modèle de polaire, de l'import (NMEA/VDR) et de la config bateau.
 *
 * Ne dépend que de glib (gboolean, g_*) et sqlite3 : aucun toolkit graphique.
 * L'interface est le serveur web (web/server.c), qui se lie à ce cœur.
 */
#ifndef LIBPOLAR_H
#define LIBPOLAR_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sqlite3.h>
#include <glib.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#ifndef MAX_PATH
#ifdef PATH_MAX
#define MAX_PATH PATH_MAX
#else
#define MAX_PATH 4096
#endif
#endif
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
#define BOAT_PATH_LEN 512   // longueur max d'un chemin de bateau/config
#define BOAT_RECENT_MAX 8   // nb de bateaux récents conservés

typedef struct {
    char name[64];
    char mains[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_mains;
    char heads[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_heads;
    char seas[BOAT_MAX_ITEMS][BOAT_TERM_LEN];  int n_seas;
} PolarDef;

typedef struct {
    char name[128];
    char mainsail[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_mainsail;
    char headsail[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_headsail;
    char seastate[BOAT_MAX_ITEMS][BOAT_TERM_LEN]; int n_seastate;
    char kw_moteur[BOAT_TERM_LEN];
    char kw_charge[BOAT_TERM_LEN];
    PolarDef polars[BOAT_MAX_POLARS]; int n_polars;
} BoatConfig;

extern BoatConfig g_boat_config;
extern char g_boat_config_path[];

typedef struct {
    double tws, twa, bsp;
    bool has_tws, has_twa, has_bsp;
    double sog;
    bool has_sog;
    double twd;       // direction vraie du vent (MWD) ; TWA = TWD - cap
    bool has_twd;
    double heading;   // cap vrai (HDT/HDG/VHW)
    bool has_heading;
    bool has_mwv_true;   // MWV,T (vent vrai rapporté à l'eau) reçu : prioritaire sur MWD (fond)
} nmea_data_t;

typedef struct {
    double offset;
    bool have_offset;
} stw_sog_filter_t;

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

typedef struct {
    polar_grid_t *grids;
    const PolarDef *defs;
    int n;
} polar_router_t;

extern polar_router_t *g_polar_router;

// Visualisation live (superposition des points bruts sur le diagramme).
extern polar_grid_t *g_live_grid;
extern double g_live_cur_twa, g_live_cur_bsp;

// Progression d'un traitement long, abstraite de l'UI : `update` est appelé
// périodiquement (une interface peut y afficher l'avancement ; le serveur web
// passe NULL). cancel_flag (optionnel) permet d'annuler.
typedef struct {
    void (*update)(void *ud, const char *msg);
    void *ud;
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

// ---- Prototypes cœur ----

// boat_config.c
void boat_config_init(BoatConfig *c);
char *boat_str_trim(char *s);
void boat_list_add(char list[][BOAT_TERM_LEN], int *n, const char *term);
bool boat_config_load(BoatConfig *c, const char *path);
bool boat_config_save(const BoatConfig *c, const char *path);
const char *sea_state_label(const char *fr, Language lang);
bool boat_find_config(const char *folder, char *out, size_t outsz);
int boat_recent_load(char list[][BOAT_PATH_LEN], int max);
void boat_recent_add(const char *boat_folder);
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
// NMEA 2000 (texte YDRAW) : même contrat que parse_nmea_sentence
bool n2k_apply_frame(int pgn, const uint8_t *d, int len, nmea_data_t *data);
int n2k_pgn_from_id(uint32_t id);
bool parse_ydraw_line(const char *line, nmea_data_t *data);
bool parse_nav_line(const char *line, nmea_data_t *data);   // 0183 ou YDRAW, détecté
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

#endif // LIBPOLAR_H
