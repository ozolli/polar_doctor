#include "polar_doctor.h"

void on_export_pdf_clicked(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

#ifdef _WIN32
    // Utiliser le dialogue natif Windows pour sauvegarder
    OPENFILENAMEW ofn;
    wchar_t filename[MAX_PATH];

    // Extraire le nom de base du fichier polaire (sans chemin ni extension)
    char base_name[256] = "polar_diagram";
    if (app->polar_data && app->polar_data->filename[0] != '\0') {
        // Trouver le dernier slash ou backslash
        const char *last_slash = strrchr(app->polar_data->filename, '/');
        const char *last_backslash = strrchr(app->polar_data->filename, '\\');
        const char *base = app->polar_data->filename;
        if (last_slash) base = last_slash + 1;
        if (last_backslash && last_backslash > base) base = last_backslash + 1;

        // Copier le nom de base
        strncpy(base_name, base, sizeof(base_name) - 1);
        base_name[sizeof(base_name) - 1] = '\0';

        // Enlever l'extension .pol si présente
        char *dot = strrchr(base_name, '.');
        if (dot && (strcmp(dot, ".pol") == 0 || strcmp(dot, ".POL") == 0)) {
            *dot = '\0';
        }
    }

    // Ajouter .pdf et convertir en wchar_t
    char pdf_name[MAX_PATH];
    snprintf(pdf_name, sizeof(pdf_name), "%s.pdf", base_name);
    MultiByteToWideChar(CP_UTF8, 0, pdf_name, -1, filename, MAX_PATH);

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"pdf";

    if (GetSaveFileNameW(&ofn)) {
        // Convertir wchar_t* en char*
        char pdf_path[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, pdf_path, sizeof(pdf_path), NULL, NULL);

        GtkPrintOperation *print = gtk_print_operation_new();

        // Définir le titre du document (juste le nom de base, sans chemin)
        gtk_print_operation_set_job_name(print, base_name);

        gtk_print_operation_set_export_filename(print, pdf_path);

        g_signal_connect(print, "draw-page", G_CALLBACK(print_page), app);
        g_signal_connect(print, "begin-print", G_CALLBACK(print_begin), app);

        GError *error = NULL;
        GtkPrintOperationResult result = gtk_print_operation_run(print,
                                                                  GTK_PRINT_OPERATION_ACTION_EXPORT,
                                                                  GTK_WINDOW(app->window),
                                                                  &error);

        if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s%s",
                                                              TR(app, "Erreur d'export PDF: ", "PDF export error: "),
                                                              error->message);
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
            g_error_free(error);
        } else {
            GtkWidget *success_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                                GTK_DIALOG_MODAL,
                                                                GTK_MESSAGE_INFO,
                                                                GTK_BUTTONS_OK,
                                                                "%s\n%s",
                                                                TR(app, "PDF exporté avec succès:", "PDF exported successfully:"),
                                                                pdf_path);
            gtk_dialog_run(GTK_DIALOG(success_dialog));
            gtk_widget_destroy(success_dialog);
        }

        g_object_unref(print);
    }
#else
    // Pour Linux, utiliser le dialogue GTK standard

    // Extraire le nom de base du fichier polaire (sans chemin ni extension)
    char base_name[256] = "polar_diagram";
    if (app->polar_data && app->polar_data->filename[0] != '\0') {
        // Trouver le dernier slash ou backslash
        const char *last_slash = strrchr(app->polar_data->filename, '/');
        const char *last_backslash = strrchr(app->polar_data->filename, '\\');
        const char *base = app->polar_data->filename;
        if (last_slash) base = last_slash + 1;
        if (last_backslash && last_backslash > base) base = last_backslash + 1;

        // Copier le nom de base
        strncpy(base_name, base, sizeof(base_name) - 1);
        base_name[sizeof(base_name) - 1] = '\0';

        // Enlever l'extension .pol si présente
        char *dot = strrchr(base_name, '.');
        if (dot && (strcmp(dot, ".pol") == 0 || strcmp(dot, ".POL") == 0)) {
            *dot = '\0';
        }
    }

    // Nom du fichier PDF par défaut
    char pdf_default_name[MAX_PATH];
    snprintf(pdf_default_name, sizeof(pdf_default_name), "%s.pdf", base_name);

    GtkWidget *dialog = gtk_file_chooser_dialog_new(TR(app, "Exporter en PDF", "Export to PDF"),
                                                      GTK_WINDOW(app->window),
                                                      GTK_FILE_CHOOSER_ACTION_SAVE,
                                                      TR(app, "_Annuler", "_Cancel"), GTK_RESPONSE_CANCEL,
                                                      TR(app, "_Enregistrer", "_Save"), GTK_RESPONSE_ACCEPT,
                                                      NULL);

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), pdf_default_name);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF files");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *pdf_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

        GtkPrintOperation *print = gtk_print_operation_new();

        // Définir le titre du document (juste le nom de base, sans chemin)
        gtk_print_operation_set_job_name(print, base_name);

        gtk_print_operation_set_export_filename(print, pdf_path);

        g_signal_connect(print, "draw-page", G_CALLBACK(print_page), app);
        g_signal_connect(print, "begin-print", G_CALLBACK(print_begin), app);

        GError *error = NULL;
        GtkPrintOperationResult result = gtk_print_operation_run(print,
                                                                  GTK_PRINT_OPERATION_ACTION_EXPORT,
                                                                  GTK_WINDOW(app->window),
                                                                  &error);

        if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
            GtkWidget *error_dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                              GTK_DIALOG_MODAL,
                                                              GTK_MESSAGE_ERROR,
                                                              GTK_BUTTONS_OK,
                                                              "%s%s",
                                                              TR(app, "Erreur d'export PDF: ", "PDF export error: "),
                                                              error->message);
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
            g_error_free(error);
        }

        g_free(pdf_path);
        g_object_unref(print);
    }

    gtk_widget_destroy(dialog);
#endif
}

#ifdef _WIN32
// Fonction pour obtenir le facteur d'échelle (permet override par variable d'env)
double get_print_scale(void) {
    const char *scale_env = getenv("POLAR_PRINT_SCALE");
    if (scale_env) {
        double scale = atof(scale_env);
        if (scale > 0.5 && scale < 3.0) {
            return scale;
        }
    }
    return WINDOWS_PRINT_SCALE;
}
#endif

// Fonction helper pour définir la taille de police avec scaling Windows
// Applique uniquement aux polices (pas au diagramme/layout)
void set_print_font_size(cairo_t *cr, double size) {
#ifdef _WIN32
    double scale = get_print_scale();
    cairo_set_font_size(cr, size * scale);
#else
    cairo_set_font_size(cr, size);
#endif
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
    set_print_font_size(cr, 12);
    cairo_move_to(cr, 10, 15);

    // Extraire le nom du fichier sans chemin et sans extension .pol
    char title[256];
    if (strlen(data->filename) > 0) {
        // Trouver le dernier slash ou backslash (Windows/Linux)
        const char *last_slash = strrchr(data->filename, '/');
        const char *last_backslash = strrchr(data->filename, '\\');
        const char *basename = data->filename;
        if (last_slash) basename = last_slash + 1;
        if (last_backslash && last_backslash > basename) basename = last_backslash + 1;

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

    set_print_font_size(cr, 7);

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
    set_print_font_size(cr, 12);
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
        set_print_font_size(cr, 7);
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
            set_print_font_size(cr, 8);
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
    set_print_font_size(cr, 9);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_show_text(cr, "VMG (Velocity Made Good)");

    vmg_y += 15;

    // En-têtes du tableau VMG
    set_print_font_size(cr, 7);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_show_text(cr, "TWS (kn)");
    cairo_move_to(cr, vmg_x + 40, vmg_y);
    cairo_show_text(cr, TR(app, "max VMG up", "max VMG up"));
    cairo_move_to(cr, vmg_x + 110, vmg_y);
    cairo_show_text(cr, TR(app, "max VMG down", "max VMG down"));

    vmg_y += 12;

    // Ligne de séparation
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, vmg_x, vmg_y);
    cairo_line_to(cr, vmg_x + 180, vmg_y);
    cairo_stroke(cr);

    vmg_y += 10;  // Plus d'espace après la ligne de séparation

    // Calculer et afficher VMG pour chaque TWS
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    set_print_font_size(cr, 7);

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

