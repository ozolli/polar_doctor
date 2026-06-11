#include "polar_doctor.h"

void draw_polar_curve(cairo_t *cr, const double *px, const double *py,
                             const double *ang, int n, double a_up, double a_dn,
                             double ur, double ug, double ub) {
    if (n < 2) return;
    cairo_set_line_width(cr, 2.0);
    for (int i = 0; i < n - 1; i++) {
        double x1 = px[i], y1 = py[i], x2 = px[i + 1], y2 = py[i + 1];
        double x0 = (i == 0) ? 2 * x1 - x2 : px[i - 1];
        double y0 = (i == 0) ? 2 * y1 - y2 : py[i - 1];
        double x3 = (i == n - 2) ? 2 * x2 - x1 : px[i + 2];
        double y3 = (i == n - 2) ? 2 * y2 - y1 : py[i + 2];
        double t = 0.5;
        double midang = (ang[i] + ang[i + 1]) * 0.5;
        if (midang >= a_up && midang <= a_dn)
            cairo_set_source_rgb(cr, ur, ug, ub);      // VMG utile : couleur de la TWS
        else
            cairo_set_source_rgb(cr, 0.9, 0.0, 0.0);   // VMG dégradé : rouge
        cairo_new_path(cr);
        cairo_move_to(cr, x1, y1);
        cairo_curve_to(cr, x1 + (x2 - x0) * t / 6.0, y1 + (y2 - y0) * t / 6.0,
                           x2 - (x3 - x1) * t / 6.0, y2 - (y3 - y1) * t / 6.0, x2, y2);
        cairo_stroke(cr);
    }
}

// Trace la courbe d'une TWS donnée : points aux TWA de la polaire + points-frontières
// exactement aux angles VMG optimaux (a_up, a_dn) pour que la bascule rouge/vert tombe
// précisément à l'angle calculé (cohérent avec le tableau VMG). useful color = (ur,ug,ub).
void draw_tws_curve(cairo_t *cr, PolarData *data, double tws,
                           int center_x, int center_y, double radius, int max_scale,
                           double ur, double ug, double ub) {
    double a_up, a_dn;
    vmg_optimal_angles(data, tws, &a_up, &a_dn);

    // Angles d'échantillonnage : rangées présentes + frontières VMG, triés
    double angs[MAX_ANGLES + 2];
    int m = 0;
    for (int i = 0; i < data->num_angles; i++)
        if (data->twa_present[i]) angs[m++] = data->twa_values[i];
    angs[m++] = a_up;
    angs[m++] = a_dn;
    for (int i = 0; i < m - 1; i++)
        for (int j = i + 1; j < m; j++)
            if (angs[i] > angs[j]) { double t = angs[i]; angs[i] = angs[j]; angs[j] = t; }

    // Points (x,y) avec BS interpolée, en évitant les doublons d'angle
    double px[MAX_ANGLES + 2], py[MAX_ANGLES + 2], ang[MAX_ANGLES + 2];
    int n = 0;
    for (int i = 0; i < m; i++) {
        if (n > 0 && fabs(angs[i] - ang[n - 1]) < 1e-6) continue;
        double bsp = interpolate_polar_bsp(data, angs[i], tws);
        if (bsp < 0.01) continue;
        double rad = angs[i] * M_PI / 180.0;
        double r = (bsp / max_scale) * radius;
        px[n] = center_x + r * sin(rad);
        py[n] = center_y - r * cos(rad);
        ang[n] = angs[i];
        n++;
    }
    draw_polar_curve(cr, px, py, ang, n, a_up, a_dn, ur, ug, ub);
}

gboolean draw_polar_diagram(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
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

    // En mode dynamique, la TWS provient du champ libre (interpolation)
    double dyn_tws = app->dynamic_mode
        ? gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->dynamic_tws_spin)) : 0.0;

    // Trouver la vitesse maximale du bateau (BSP) pour l'échelle
    double max_bsp = 0.0;
    if (app->dynamic_mode) {
        max_bsp = dynamic_curve_max(data, dyn_tws, NULL);
    } else {
        for (int i = 0; i < MAX_ANGLES; i++) {
            for (int j = tws_from_idx; j <= tws_to_idx && j < data->num_speeds; j++) {
                if (data->polar_data[i][j] > max_bsp) {
                    max_bsp = data->polar_data[i][j];
                }
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

    // --- Mode dynamique : courbe interpolée unique + ligne bleue au glissé ---
    if (app->dynamic_mode) {
        if (dyn_tws > 0.0 && max_bsp > 0.01) {
            draw_tws_curve(cr, data, dyn_tws, center_x, center_y, radius, max_scale,
                           0.0, 0.8, 0.0);  // partie utile en vert

            // Ligne bleue : uniquement pendant le clic maintenu
            if (app->dragging) {
                double bsp = interpolate_polar_bsp(data, app->drag_twa, dyn_tws);
                double rad = app->drag_twa * M_PI / 180.0;
                double r = (bsp / max_scale) * radius;
                cairo_set_source_rgb(cr, 0.0, 0.0, 1.0);
                cairo_set_line_width(cr, 3.0);
                cairo_new_path(cr);
                cairo_move_to(cr, center_x, center_y);
                cairo_line_to(cr, center_x + r * sin(rad), center_y - r * cos(rad));
                cairo_stroke(cr);
            }
        }
        return FALSE;
    }

    // Dessiner les courbes polaires pour chaque TWS (couleur par TWS = VMG utile, rouge = dégradé)
    for (int speed_idx = tws_from_idx; speed_idx <= tws_to_idx && speed_idx < data->num_speeds; speed_idx++) {
        int tws = data->tws_values[speed_idx];
        if (tws == 0) continue;  // Ne pas dessiner TWS 0

        double cr_, cg_, cb_;
        tws_palette_color(speed_idx, &cr_, &cg_, &cb_);
        draw_tws_curve(cr, data, tws, center_x, center_y, radius, max_scale, cr_, cg_, cb_);
    }

    return FALSE;
}

// TWA correspondant à un point cliqué sur le diagramme (0° en haut, 180° en bas)
double diagram_event_twa(GtkWidget *w, double ex, double ey) {
    int height = gtk_widget_get_allocated_height(w);
    int margin = 80, center_x = margin, center_y = height / 2;
    double dx = ex - center_x, dy = center_y - ey;
    double ang = atan2(dx, dy) * 180.0 / M_PI;
    if (ang < 0) ang = 0;
    if (ang > 180) ang = 180;
    return ang;
}

// Met à jour le panneau du mode dynamique : valeurs live pendant le glissé, résumé sinon
void update_dynamic_info(AppWidgets *app) {
    if (!app->dynamic_info) return;
    if (!app->dynamic_mode) { gtk_label_set_text(GTK_LABEL(app->dynamic_info), ""); return; }

    PolarData *data = app->polar_data;
    double tws = gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->dynamic_tws_spin));
    char buf[256];

    if (app->dragging) {
        double twa = app->drag_twa;
        double bs = interpolate_polar_bsp(data, twa, tws);
        double tr = twa * M_PI / 180.0;
        double aws = sqrt(tws * tws + bs * bs + 2.0 * tws * bs * cos(tr));
        double awa = atan2(tws * sin(tr), bs + tws * cos(tr)) * 180.0 / M_PI;
        double vmg = bs * cos(tr);
        snprintf(buf, sizeof(buf),
                 "TWA   %.0f°\nAWA  %.2f°   AWS  %.2f kts\nBS %.2f kts   VMG %.2f kts",
                 twa, awa, aws, bs, vmg);
    } else {
        double cmax_twa = 0, amax_tws = 0, amax_twa = 0;
        double cmax = dynamic_curve_max(data, tws, &cmax_twa);
        double amax = polar_absolute_max(data, &amax_tws, &amax_twa);
        snprintf(buf, sizeof(buf),
                 TR(app, "Vitesse max: %.2f nds à %.2f°\nVitesse max absolue: %.2f nds\nTWS:%.2f nds TWA:%.2f°",
                         "Max speed: %.2f kn at %.2f°\nAbsolute max speed: %.2f kn\nTWS:%.2f kn TWA:%.2f°"),
                 cmax, cmax_twa, amax, amax_tws, amax_twa);
    }
    gtk_label_set_text(GTK_LABEL(app->dynamic_info), buf);
}

gboolean on_diagram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (!app->dynamic_mode || event->button != 1) return FALSE;
    app->dragging = TRUE;
    app->drag_twa = round(diagram_event_twa(widget, event->x, event->y));
    update_dynamic_info(app);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean on_diagram_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (!app->dynamic_mode || !app->dragging) return FALSE;
    app->drag_twa = round(diagram_event_twa(widget, event->x, event->y));
    update_dynamic_info(app);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean on_diagram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    if (event->button != 1) return FALSE;
    app->dragging = FALSE;
    update_dynamic_info(app);  // bascule sur le résumé
    gtk_widget_queue_draw(widget);
    return TRUE;
}

void on_dynamic_toggled(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    app->dynamic_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    app->dragging = FALSE;
    gtk_widget_set_visible(app->dynamic_tws_label, app->dynamic_mode);
    gtk_widget_set_visible(app->dynamic_tws_spin, app->dynamic_mode);
    update_dynamic_info(app);
    rebuild_legend(app);  // masquer/afficher la légende selon le mode
    gtk_widget_queue_draw(app->polar_view);
}

void on_dynamic_tws_changed(GtkWidget *widget, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    (void)widget;
    update_dynamic_info(app);
    gtk_widget_queue_draw(app->polar_view);
}

// Recréer complètement l'onglet données (pour changement de colonnes)
