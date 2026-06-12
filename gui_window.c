#include "polar_doctor.h"

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
            int last_idx = tws_default_to_index(app->polar_data);
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
            int last_idx = tws_default_to_index(app->polar_data);
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

        if (new_tws >= 0 && new_tws <= 100) {
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
                int last_idx = tws_default_to_index(app->polar_data);
                gtk_combo_box_set_active(GTK_COMBO_BOX(app->tws_to_combo), last_idx);
            }
        } else {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s", TR(app, "La vitesse TWS doit être entre 0 et 100 kn.", "TWS speed must be between 0 and 100 kn."));
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
            int last_idx = tws_default_to_index(app->polar_data);
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

// Dessine un petit drapeau (24x16) via Cairo et le renvoie en pixbuf. On évite les
// emojis-drapeaux : Windows (Segoe UI Emoji) les rend en lettres « FR » / « GB ».
static GdkPixbuf *make_flag_pixbuf(Language lang) {
    const int w = 24, h = 16;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    if (lang == LANG_FR) {
        cairo_set_source_rgb(cr, 0.00, 0.33, 0.64); cairo_rectangle(cr, 0, 0, w/3.0, h); cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);          cairo_rectangle(cr, w/3.0, 0, w/3.0, h); cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.94, 0.26, 0.21); cairo_rectangle(cr, 2*w/3.0, 0, w/3.0, h); cairo_fill(cr);
    } else {
        // Union Jack simplifié mais reconnaissable
        cairo_set_source_rgb(cr, 0.00, 0.13, 0.41); cairo_paint(cr);
        cairo_set_line_width(cr, 4); cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, 0, 0); cairo_line_to(cr, w, h);
        cairo_move_to(cr, w, 0); cairo_line_to(cr, 0, h); cairo_stroke(cr);
        cairo_set_line_width(cr, 1.5); cairo_set_source_rgb(cr, 0.78, 0.06, 0.18);
        cairo_move_to(cr, 0, 0); cairo_line_to(cr, w, h);
        cairo_move_to(cr, w, 0); cairo_line_to(cr, 0, h); cairo_stroke(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_rectangle(cr, w/2.0 - 3, 0, 6, h); cairo_fill(cr);
        cairo_rectangle(cr, 0, h/2.0 - 3, w, 6); cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.78, 0.06, 0.18);
        cairo_rectangle(cr, w/2.0 - 1.5, 0, 3, h); cairo_fill(cr);
        cairo_rectangle(cr, 0, h/2.0 - 1.5, w, 3); cairo_fill(cr);
    }
    cairo_destroy(cr);
    GdkPixbuf *pix = gdk_pixbuf_get_from_surface(s, 0, 0, w, h);
    cairo_surface_destroy(s);
    return pix;
}

// Pose le drapeau de la langue courante sur le bouton de langue.
static void set_lang_button_flag(AppWidgets *app) {
    GdkPixbuf *pix = make_flag_pixbuf(app->language);
    GtkWidget *img = gtk_image_new_from_pixbuf(pix);
    if (pix) g_object_unref(pix);
    gtk_button_set_image(GTK_BUTTON(app->lang_button), img);
    gtk_button_set_always_show_image(GTK_BUTTON(app->lang_button), TRUE);
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
    app->btn_export_pdf = gtk_button_new_with_label(TR(app, "Export PDF", "Export PDF"));

    g_signal_connect(app->btn_open, "clicked", G_CALLBACK(on_open_clicked), app);
    g_signal_connect(app->btn_save, "clicked", G_CALLBACK(on_save_clicked), app);
    g_signal_connect(app->btn_create, "clicked", G_CALLBACK(on_create_clicked), app);
    g_signal_connect(app->btn_update, "clicked", G_CALLBACK(on_update_clicked), app);
    g_signal_connect(app->btn_export_pdf, "clicked", G_CALLBACK(on_export_pdf_clicked), app);

    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_open, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_create, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_update, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_export_pdf, FALSE, FALSE, 0);

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

    // Contrôle du percentile d'agrégation (appliqué à la prochaine génération / mise à jour)
    app->percentile_label = gtk_label_new(TR(app, "Percentile :", "Percentile:"));
    gtk_box_pack_start(GTK_BOX(toolbar), app->percentile_label, FALSE, FALSE, 0);
    app->percentile_spin = gtk_spin_button_new_with_range(85, 95, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->percentile_spin), g_polar_percentile);
    gtk_widget_set_tooltip_text(app->percentile_spin,
        TR(app, "Percentile de vitesse retenu par cellule lors de la génération/mise à jour "
                "(plus haut = polaire plus ambitieuse). Prend effet à la prochaine génération.",
               "Boat-speed percentile kept per cell when generating/updating "
               "(higher = more aggressive polar). Takes effect on next generation."));
    g_signal_connect(app->percentile_spin, "value-changed", G_CALLBACK(on_percentile_changed), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->percentile_spin, FALSE, FALSE, 0);

    // Séparateur
    GtkWidget *separator3 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator3, FALSE, FALSE, 5);

    // Bouton Configuration du bateau (inventaire voiles / états de mer)
    app->btn_boat = gtk_button_new_with_label(TR(app, "Bateau…", "Boat…"));
    gtk_widget_set_tooltip_text(app->btn_boat,
        TR(app, "Configurer l'inventaire du bateau : grand-voile, voiles d'avant, "
                "états de mer, mots-clés moteur.",
               "Configure the boat inventory: mainsail, headsails, sea states, "
               "engine keywords."));
    g_signal_connect(app->btn_boat, "clicked", G_CALLBACK(on_boat_config_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_boat, FALSE, FALSE, 0);

    // Bouton Aide
    app->btn_help = gtk_button_new_with_label(TR(app, "Aide", "Help"));
    g_signal_connect(app->btn_help, "clicked", G_CALLBACK(on_help_clicked), app);
    gtk_box_pack_start(GTK_BOX(toolbar), app->btn_help, FALSE, FALSE, 0);

    // Bouton de langue avec drapeau français (par défaut) - tout à droite
    app->lang_button = gtk_button_new();
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
    set_lang_button_flag(app);
}

void on_lang_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    // Basculer la langue
    app->language = (app->language == LANG_FR) ? LANG_EN : LANG_FR;
    set_lang_button_flag(app);

    // Mettre à jour l'interface
    update_interface_language(app);
}

void on_percentile_changed(GtkWidget *widget, gpointer user_data) {
    (void)user_data;
    g_polar_percentile = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
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
    gtk_button_set_label(GTK_BUTTON(app->btn_export_pdf), TR(app, "Export PDF", "Export PDF"));
    gtk_button_set_label(GTK_BUTTON(app->btn_add_twa), TR(app, "Ajout TWA", "Add TWA"));
    gtk_button_set_label(GTK_BUTTON(app->btn_add_tws), TR(app, "Ajout TWS", "Add TWS"));
    gtk_button_set_label(GTK_BUTTON(app->btn_delete), TR(app, "Suppression", "Delete"));
    gtk_button_set_label(GTK_BUTTON(app->btn_help), TR(app, "Aide", "Help"));
    gtk_label_set_text(GTK_LABEL(app->percentile_label), TR(app, "Percentile :", "Percentile:"));
    gtk_button_set_label(GTK_BUTTON(app->dynamic_check), TR(app, "Mode dynamique", "Dynamic mode"));
    update_dynamic_info(app);  // rafraîchir le résumé dans la langue courante

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
        "  - Le curseur 'Percentile' (barre d'outils) règle l'agrégation (P85–P95, défaut P90)\n"
        "  - Lissage glissant du STW à l'import NMEA (anti-bruit du loch)\n"
        "  - Débruitage du STW par comparaison au SOG : les sauts du loch (coque déjaugée,\n"
        "    roue à aube bloquée) sont rejetés ; un courant lent est préservé\n"
        "  - Filtre moteur : points moteur (RPM > 0) exclus, sauf charge batteries (voir VDR)\n"
        "  - Affiche la progression pendant le traitement\n\n"
        "<b>• Mettre à jour</b>\n"
        "  Met à jour la polaire actuelle avec de nouvelles données NMEA/VDR\n"
        "  - Sélection multiple possible\n"
        "  - Seules les performances égales ou supérieures sont conservées\n"
        "  - Les données inférieures à 95% de l'existant sont filtrées\n\n"
        "<b>• Export PDF</b>\n"
        "  Exporte la polaire (tableau de données + diagramme + VMG) au format PDF\n"
        "  - Le nom du fichier est automatiquement suggéré (basé sur le nom de la polaire)\n"
        "  - Sous Windows : Dialogues natifs pour meilleure stabilité\n"
        "  - Taille du texte ajustable via variable POLAR_PRINT_SCALE (0.5 à 3.0)\n\n"
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
        "  - Sélectionner la plage de TWS à afficher (de/à) ; une couleur par TWS + légende\n"
        "  - Les courbes sont lissées avec interpolation Catmull-Rom\n"
        "  - Vert = plage VMG utile, rouge = VMG dégradé (près trop serré / portant trop bas)\n"
        "  - TWS 0 est exclu de l'affichage\n"
        "  <b>Mode dynamique</b> (case à cocher) :\n"
        "  - Saisir une TWS au clavier (boutons ±1 kn, décimales possibles, ex. 9,85)\n"
        "  - Affiche la seule courbe interpolée pour cette TWS\n"
        "  - Clic gauche maintenu / glissé sur le diagramme : ligne bleue suivant le curseur,\n"
        "    lecture en direct TWA / AWA / AWS / BS / VMG (TWA au degré entier)\n"
        "  - Au relâchement : vitesse max de la courbe et vitesse max absolue de la polaire\n\n"
        "<b>• VMG</b>\n"
        "  Tableau des meilleures performances VMG (Velocity Made Good)\n"
        "  - VMG up : meilleur angle au près\n"
        "  - VMG down : meilleur angle au portant\n"
        "  - Calculé pour chaque vitesse de vent (TWS)\n\n"
        "<b>FORMAT DES FICHIERS</b>\n\n"
        "<b>• Fichiers NMEA (.txt, .nmea, .log)</b>\n"
        "  Nécessite les sentences MWV (vent) et VHW (vitesse surface)\n"
        "  SOG lu s'il est présent (RMC, VTG, VBW, RMA, OSD) pour débruiter le STW\n"
        "  Les données sont groupées par buckets de 5° (TWA) et 2 kn (TWS)\n\n"
        "<b>• Fichiers VDR (.db)</b>\n"
        "  Base SQLite générée par qtVlm (TWA, TWS, STW ; SOG, RPM, COMMENT si présents)\n"
        "  Filtre moteur : un point avec RPM > 0 est exclu, SAUF si le commentaire contient\n"
        "    « Charge » (moteur débrayé pour charger les batteries : le bateau navigue, on garde)\n"
        "  Débruitage du STW par le SOG quand la colonne SOG est présente\n\n"
        "<b>• Fichiers polaires (.pol)</b>\n"
        "  Format tabulaire avec séparateur point-virgule\n"
        "  Première ligne : en-têtes TWS\n"
        "  Colonnes suivantes : TWA + vitesses BSP\n\n"
        "<b>CONSEILS</b>\n\n"
        "• Combiner plusieurs sorties de navigation pour plus de précision\n"
        "• Utiliser 'Mettre à jour' pour améliorer progressivement une polaire\n"
        "• Les valeurs sont toujours affichées avec 2 décimales\n"
        "• Un minimum de 3 points est requis par cellule\n"
        "• L'agrégation retient un percentile élevé (P90 par défaut, réglable P85–P95 dans la barre d'outils)\n"
        "  afin de viser la performance atteignable plutôt que la moyenne\n"
        "• Le STW (loch) est fiabilisé par comparaison au SOG (GPS) quand il est disponible";

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
        "  - The 'Percentile' toolbar control sets aggregation (P85–P95, default P90)\n"
        "  - Sliding-window smoothing of STW on NMEA import (log noise reduction)\n"
        "  - STW denoising by comparison with SOG: log spikes (hull lifting,\n"
        "    blocked paddle wheel) are rejected; a slowly-varying current is preserved\n"
        "  - Engine filter: motoring points (RPM > 0) excluded, except battery charging (see VDR)\n"
        "  - Progress is displayed during processing\n\n"
        "<b>• Update</b>\n"
        "  Update the current polar with new NMEA/VDR data\n"
        "  - Multiple selection possible\n"
        "  - Only equal or better performances are kept\n"
        "  - Data below 95% of existing values is filtered\n\n"
        "<b>• Export PDF</b>\n"
        "  Export the polar (data table + diagram + VMG) to PDF format\n"
        "  - Filename is automatically suggested (based on polar name)\n"
        "  - On Windows: Native dialogs for better stability\n"
        "  - Text size adjustable via POLAR_PRINT_SCALE variable (0.5 to 3.0)\n\n"
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
        "  - Select the TWS range to display (from/to); one color per TWS + legend\n"
        "  - Curves are smoothed with Catmull-Rom interpolation\n"
        "  - Green = useful VMG range, red = degraded VMG (too tight upwind / too low downwind)\n"
        "  - TWS 0 is excluded from display\n"
        "  <b>Dynamic mode</b> (checkbox):\n"
        "  - Enter a TWS via keyboard (± buttons step 1 kn, decimals allowed, e.g. 9.85)\n"
        "  - Shows only the interpolated curve for that TWS\n"
        "  - Press and hold / drag on the diagram: blue line tracks the cursor,\n"
        "    live readout of TWA / AWA / AWS / BS / VMG (TWA at whole degrees)\n"
        "  - On release: curve max speed and the polar's absolute max speed\n\n"
        "<b>• VMG</b>\n"
        "  Table of best VMG (Velocity Made Good) performances\n"
        "  - VMG up: best upwind angle\n"
        "  - VMG down: best downwind angle\n"
        "  - Calculated for each wind speed (TWS)\n\n"
        "<b>FILE FORMATS</b>\n\n"
        "<b>• NMEA files (.txt, .nmea, .log)</b>\n"
        "  Requires MWV (wind) and VHW (speed through water) sentences\n"
        "  SOG read when present (RMC, VTG, VBW, RMA, OSD) to denoise STW\n"
        "  Data is grouped by 5° (TWA) and 2 kn (TWS) buckets\n\n"
        "<b>• VDR files (.db)</b>\n"
        "  SQLite database generated by qtVlm (TWA, TWS, STW; SOG, RPM, COMMENT if present)\n"
        "  Engine filter: a point with RPM > 0 is excluded, EXCEPT when the comment contains\n"
        "    \"Charge\" (engine in neutral to charge batteries: the boat is sailing, kept)\n"
        "  STW denoised using SOG when the SOG column is present\n\n"
        "<b>• Polar files (.pol)</b>\n"
        "  Tabular format with semicolon separator\n"
        "  First row: TWS headers\n"
        "  Following columns: TWA + BSP speeds\n\n"
        "<b>TIPS</b>\n\n"
        "• Combine multiple sailing sessions for better accuracy\n"
        "• Use 'Update' to progressively improve a polar\n"
        "• Values are always displayed with 2 decimals\n"
        "• Minimum of 3 points required per cell\n"
        "• Aggregation keeps a high percentile (P90 by default, adjustable P85–P95 in the toolbar)\n"
        "  to target achievable performance rather than the average\n"
        "• STW (log) is made more reliable by comparison with SOG (GPS) when available";

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

// --- Éditeur GUI de la configuration du bateau ---
void boat_textview_set(GtkTextView *tv, char list[][BOAT_TERM_LEN], int n) {
    GString *s = g_string_new("");
    for (int i = 0; i < n; i++) { g_string_append(s, list[i]); g_string_append_c(s, '\n'); }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(tv), s->str, -1);
    g_string_free(s, TRUE);
}

int boat_textview_get(GtkTextView *tv, char list[][BOAT_TERM_LEN]) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    char *txt = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line && n < BOAT_MAX_ITEMS;
         line = strtok_r(NULL, "\n", &save)) {
        char *t = boat_str_trim(line);
        if (*t) { snprintf(list[n], BOAT_TERM_LEN, "%s", t); n++; }
    }
    g_free(txt);
    return n;
}

GtkWidget *boat_make_list_editor(const char *title, GtkWidget **out_tv) {
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sw, 180, 150);
    GtkWidget *tv = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_container_add(GTK_CONTAINER(frame), sw);
    *out_tv = tv;
    return frame;
}

// Recopie les champs du dialogue dans g_boat_config (édition de session).
void boat_dialog_collect(GtkWidget *name_e, GtkWidget *mot_e, GtkWidget *chg_e,
                                GtkTextView *tv_main, GtkTextView *tv_head, GtkTextView *tv_sea) {
    snprintf(g_boat_config.name, sizeof(g_boat_config.name), "%s", gtk_entry_get_text(GTK_ENTRY(name_e)));
    snprintf(g_boat_config.kw_moteur, BOAT_TERM_LEN, "%s", gtk_entry_get_text(GTK_ENTRY(mot_e)));
    snprintf(g_boat_config.kw_charge, BOAT_TERM_LEN, "%s", gtk_entry_get_text(GTK_ENTRY(chg_e)));
    g_boat_config.n_mainsail = boat_textview_get(tv_main, g_boat_config.mainsail);
    g_boat_config.n_headsail = boat_textview_get(tv_head, g_boat_config.headsail);
    g_boat_config.n_seastate = boat_textview_get(tv_sea,  g_boat_config.seastate);
}

void on_boat_config_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppWidgets *app = (AppWidgets *)user_data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        TR(app, "Configuration du bateau", "Boat configuration"),
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        TR(app, "Charger…", "Load…"), 1,
        TR(app, "Enregistrer…", "Save…"), 2,
        TR(app, "Fermer", "Close"), GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 660, 470);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 6);

    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(name_box),
        gtk_label_new(TR(app, "Nom du bateau :", "Boat name:")), FALSE, FALSE, 0);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), g_boat_config.name);
    gtk_box_pack_start(GTK_BOX(name_box), name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), name_box, FALSE, FALSE, 0);

    GtkWidget *lists = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tv_main, *tv_head, *tv_sea;
    gtk_box_pack_start(GTK_BOX(lists),
        boat_make_list_editor(TR(app, "Grand-voile (états)", "Mainsail (states)"), &tv_main), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(lists),
        boat_make_list_editor(TR(app, "Voiles d'avant", "Headsails"), &tv_head), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(lists),
        boat_make_list_editor(TR(app, "États de mer", "Sea states"), &tv_sea), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), lists, TRUE, TRUE, 0);

    GtkWidget *hint = gtk_label_new(
        TR(app, "Un terme par ligne, tel qu'écrit dans les commentaires (un tag voile termine le mode moteur).",
               "One term per line, as written in the comments (a sail tag ends engine mode)."));
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);

    GtkWidget *eng = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(eng), gtk_label_new(TR(app, "Moteur :", "Engine:")), FALSE, FALSE, 0);
    GtkWidget *mot_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(mot_entry), g_boat_config.kw_moteur);
    gtk_box_pack_start(GTK_BOX(eng), mot_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(eng), gtk_label_new(TR(app, "Charge :", "Charge:")), FALSE, FALSE, 0);
    GtkWidget *chg_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(chg_entry), g_boat_config.kw_charge);
    gtk_box_pack_start(GTK_BOX(eng), chg_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), eng, FALSE, FALSE, 0);

    boat_textview_set(GTK_TEXT_VIEW(tv_main), g_boat_config.mainsail, g_boat_config.n_mainsail);
    boat_textview_set(GTK_TEXT_VIEW(tv_head), g_boat_config.headsail, g_boat_config.n_headsail);
    boat_textview_set(GTK_TEXT_VIEW(tv_sea),  g_boat_config.seastate, g_boat_config.n_seastate);

    gtk_widget_show_all(dialog);

    int resp;
    while ((resp = gtk_dialog_run(GTK_DIALOG(dialog))) == 1 || resp == 2) {
        boat_dialog_collect(name_entry, mot_entry, chg_entry,
                            GTK_TEXT_VIEW(tv_main), GTK_TEXT_VIEW(tv_head), GTK_TEXT_VIEW(tv_sea));

        GtkFileChooserAction action = (resp == 2) ? GTK_FILE_CHOOSER_ACTION_SAVE
                                                  : GTK_FILE_CHOOSER_ACTION_OPEN;
        GtkWidget *fc = gtk_file_chooser_dialog_new(
            (resp == 2) ? TR(app, "Enregistrer boat.cfg", "Save boat.cfg")
                        : TR(app, "Charger boat.cfg", "Load boat.cfg"),
            GTK_WINDOW(dialog), action,
            TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
            (resp == 2) ? TR(app, "_Enregistrer", "_Save") : TR(app, "_Ouvrir", "_Open"),
            GTK_RESPONSE_ACCEPT, NULL);
        if (resp == 2)
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), "boat.cfg");
        if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
            char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
            if (path && resp == 2) {
                boat_config_save(&g_boat_config, path);
            } else if (path && boat_config_load(&g_boat_config, path)) {
                gtk_entry_set_text(GTK_ENTRY(name_entry), g_boat_config.name);
                gtk_entry_set_text(GTK_ENTRY(mot_entry), g_boat_config.kw_moteur);
                gtk_entry_set_text(GTK_ENTRY(chg_entry), g_boat_config.kw_charge);
                boat_textview_set(GTK_TEXT_VIEW(tv_main), g_boat_config.mainsail, g_boat_config.n_mainsail);
                boat_textview_set(GTK_TEXT_VIEW(tv_head), g_boat_config.headsail, g_boat_config.n_headsail);
                boat_textview_set(GTK_TEXT_VIEW(tv_sea),  g_boat_config.seastate, g_boat_config.n_seastate);
            }
            g_free(path);
        }
        gtk_widget_destroy(fc);
    }

    boat_dialog_collect(name_entry, mot_entry, chg_entry,
                        GTK_TEXT_VIEW(tv_main), GTK_TEXT_VIEW(tv_head), GTK_TEXT_VIEW(tv_sea));
    gtk_widget_destroy(dialog);
}

