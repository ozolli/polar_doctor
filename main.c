#include "polar_doctor.h"

int g_polar_percentile = DEFAULT_POLAR_PERCENTILE;
BoatConfig g_boat_config;
char g_boat_config_path[BOAT_PATH_LEN] = "";

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
    app.dynamic_mode = FALSE;
    app.dragging = FALSE;
    app.drag_twa = 0.0;
    boat_config_init(&g_boat_config);

    create_main_window(&app);

    gtk_widget_show_all(app.window);
    gtk_main();

    return 0;
}
