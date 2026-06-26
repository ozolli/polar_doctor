/*
 * polar_doctor.h
 * Header de l'application GTK. Tire le cœur métier (libpolar.h) puis GTK/Cairo,
 * et déclare les types et prototypes spécifiques à l'interface graphique.
 * Les modules cœur (polar_data.c, import.c, boat_config.c) n'incluent QUE
 * libpolar.h et ne dépendent donc pas de GTK.
 */
#ifndef POLAR_DOCTOR_H
#define POLAR_DOCTOR_H

#include "libpolar.h"

#include <gtk/gtk.h>
#include <cairo.h>

// Configuration spécifique Windows pour les dialogues de fichiers
#ifdef _WIN32
#define GTK_FILE_CHOOSER_NATIVE_DISABLED 1
#define WINDOWS_PRINT_SCALE 1.0
#else
#define WINDOWS_PRINT_SCALE 1.0
#endif

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

// ---- Prototypes UI ----

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
