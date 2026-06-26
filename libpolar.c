/*
 * libpolar.c — définitions des variables globales du cœur métier.
 * Séparées de main.c pour que l'interface web / le moteur de capture puissent
 * se lier au cœur (polar_data + import + boat_config) sans l'appli GTK.
 */
#include "libpolar.h"

int g_polar_percentile = DEFAULT_POLAR_PERCENTILE;
BoatConfig g_boat_config;
char g_boat_config_path[BOAT_PATH_LEN] = "";
polar_router_t *g_polar_router = NULL;
polar_grid_t *g_live_grid = NULL;
double g_live_cur_twa = 0.0, g_live_cur_bsp = 0.0;
