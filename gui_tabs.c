#include "polar_doctor.h"

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
    // Événements souris pour le mode dynamique (clic maintenu + glissé)
    gtk_widget_add_events(drawing_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(G_OBJECT(drawing_area), "button-press-event", G_CALLBACK(on_diagram_button_press), app);
    g_signal_connect(G_OBJECT(drawing_area), "motion-notify-event", G_CALLBACK(on_diagram_motion), app);
    g_signal_connect(G_OBJECT(drawing_area), "button-release-event", G_CALLBACK(on_diagram_button_release), app);
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

    // Mode dynamique : case à cocher + champ TWS libre (façon qtVlm)
    GtkWidget *dyn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    app->dynamic_check = gtk_check_button_new_with_label(TR(app, "Mode dynamique", "Dynamic mode"));
    g_signal_connect(app->dynamic_check, "toggled", G_CALLBACK(on_dynamic_toggled), app);
    gtk_box_pack_start(GTK_BOX(dyn_box), app->dynamic_check, FALSE, FALSE, 0);

    app->dynamic_tws_label = gtk_label_new("TWS :");
    gtk_widget_set_no_show_all(app->dynamic_tws_label, TRUE);  // masqué tant que mode off
    gtk_box_pack_start(GTK_BOX(dyn_box), app->dynamic_tws_label, FALSE, FALSE, 0);
    app->dynamic_tws_spin = gtk_spin_button_new_with_range(0.1, 50.0, 1.0);  // +/- de 1 kn
    gtk_widget_set_no_show_all(app->dynamic_tws_spin, TRUE);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(app->dynamic_tws_spin), 2);  // décimales au clavier
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->dynamic_tws_spin), 12.0);
    gtk_widget_set_tooltip_text(app->dynamic_tws_spin,
        TR(app, "Vitesse de vent (TWS) pour la courbe interpolée. Clic maintenu sur le diagramme pour lire TWA/AWA/AWS/BS/VMG.",
               "Wind speed (TWS) for the interpolated curve. Press and hold on the diagram to read TWA/AWA/AWS/BS/VMG."));
    g_signal_connect(app->dynamic_tws_spin, "value-changed", G_CALLBACK(on_dynamic_tws_changed), app);
    gtk_box_pack_start(GTK_BOX(dyn_box), app->dynamic_tws_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_right), dyn_box, FALSE, FALSE, 0);

    // Container pour le tableau VMG (sera rempli dynamiquement)
    app->vmg_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox_right), app->vmg_container, TRUE, TRUE, 0);

    // Panneau d'information du mode dynamique (valeurs live ou résumé)
    app->dynamic_info = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->dynamic_info), 0.0);
    gtk_label_set_justify(GTK_LABEL(app->dynamic_info), GTK_JUSTIFY_LEFT);
    gtk_box_pack_start(GTK_BOX(vbox_right), app->dynamic_info, FALSE, FALSE, 0);

    // Légende des couleurs par TWS (mode multi-courbes)
    app->legend_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox_right), app->legend_container, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox_right, FALSE, FALSE, 0);

    // Colonne « Live » à droite (boutons d'annotation + démarrer/arrêter) — à côté
    // du diagramme pour annoter en regardant le nuage de points se construire.
    gtk_box_pack_start(GTK_BOX(hbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), create_live_tab(app), FALSE, FALSE, 0);

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

    rebuild_legend(app);  // garder la légende des couleurs synchronisée
}

// Reconstruit la légende des couleurs par TWS (mode multi-courbes uniquement)
void rebuild_legend(AppWidgets *app) {
    if (!app->legend_container) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(app->legend_container));
    for (GList *it = children; it != NULL; it = g_list_next(it))
        gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);

    if (app->dynamic_mode) return;  // pas de légende en mode dynamique (TWS unique)

    PolarData *data = app->polar_data;
    int combo_from = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_from_combo));
    int combo_to = gtk_combo_box_get_active(GTK_COMBO_BOX(app->tws_to_combo));
    int from = (combo_from >= 0) ? combo_index_to_tws_index(data, combo_from) : 0;
    int to = (combo_to >= 0) ? combo_index_to_tws_index(data, combo_to) : data->num_speeds - 1;
    if (from > to) { int t = from; from = to; to = t; }

    for (int i = from; i <= to && i < data->num_speeds; i++) {
        int tws = data->tws_values[i];
        if (tws == 0) continue;
        double r, g, b;
        tws_palette_color(i, &r, &g, &b);
        char markup[128];
        snprintf(markup, sizeof(markup),
                 "<span foreground=\"#%02x%02x%02x\">TWS %d kts</span>",
                 (int)(r * 255), (int)(g * 255), (int)(b * 255), tws);
        GtkWidget *lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(lbl), markup);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(app->legend_container), lbl, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(app->legend_container);
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
// Rafraîchit toute l'interface après le chargement d'une polaire (.pol) :
// tableau de données, combos TWS, table VMG, diagramme, barre d'état.
void refresh_after_polar_load(AppWidgets *app, const char *filename) {
    rebuild_data_tab(app);

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
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), tws_default_to_index(app->polar_data));

    rebuild_vmg_table(app);
    gtk_widget_queue_draw(app->polar_view);
    if (filename) gtk_statusbar_push(GTK_STATUSBAR(app->status_bar), 0, filename);
}

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
            refresh_after_polar_load(app, filename);
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

// Fonction pour exporter en PDF
