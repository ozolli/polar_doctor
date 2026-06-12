#include "polar_doctor.h"

// Capture live : on suit le VDR SQLite de qtVlm (lecture seule) et on alimente, en
// temps réel, les polaires du bateau. L'état voile/moteur/mer n'est PAS lu dans les
// commentaires : il est piloté par des boutons cliquables (vaut aussi pour le futur
// flux NMEA LAN). Chaque nouveau point est tagué avec l'état des boutons, exclu si
// « Moteur » est actif, puis routé via polar_def_matches() vers les grilles.

#define LIVE_INTERVAL_MS 3000

typedef struct {
    AppWidgets *app;
    gboolean active;
    int source;                 // 0 = VDR fichier, 1 = NMEA réseau (TCP)
    guint timer_id;
    char db_path[BOAT_PATH_LEN];
    long last_time;
    sqlite3 *db;
    // NMEA réseau (TCP : client/conn/din ; UDP : usock/net_source)
    GSocketClient *client;
    GSocketConnection *conn;
    GDataInputStream *din;
    GCancellable *cancel;
    GSocket *usock;
    guint net_source;
    nmea_data_t nmea;
    nmea_smoother_t smoother;
    polar_grid_t *grids;
    int n;
    stw_sog_filter_t stwf;
    char cur_main[BOAT_TERM_LEN], cur_head[BOAT_TERM_LEN], cur_sea[BOAT_TERM_LEN];
    gboolean moteur;
    int points;
    double last_twa, last_tws, last_stw, last_sog;
    GtkWidget *path_entry, *net_entry, *source_combo, *toggle, *readout, *controls_box;
} LiveSession;

static LiveSession L;

// --- état des boutons ---------------------------------------------------------

static void on_live_combo(GtkWidget *w, gpointer dimp) {
    int dim = GPOINTER_TO_INT(dimp);
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    char *dst = (dim == 0) ? L.cur_main : (dim == 1) ? L.cur_head : L.cur_sea;
    if (idx <= 0) { dst[0] = 0; return; }   // « (aucun) »
    char (*inv)[BOAT_TERM_LEN] = (dim == 0) ? g_boat_config.mainsail
                               : (dim == 1) ? g_boat_config.headsail : g_boat_config.seastate;
    g_strlcpy(dst, inv[idx - 1], BOAT_TERM_LEN);   // index 0 = (aucun)
}

static void on_live_moteur(GtkToggleButton *b, gpointer unused) {
    (void)unused;
    L.moteur = gtk_toggle_button_get_active(b);
}

// Un choix = un libellé + une liste déroulante (index 0 = « (aucun) »).
// dim : 0=GV, 1=voile d'avant, 2=état de mer.
static void build_radio_row(GtkWidget *box, const char *title,
                            char inv[][BOAT_TERM_LEN], int n, gboolean is_sea, int dim) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_set_size_request(lbl, 95, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);

    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), TR(L.app, "(aucun)", "(none)"));
    for (int i = 0; i < n; i++) {
        const char *disp = is_sea ? sea_state_label(inv[i], L.app->language) : inv[i];
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), disp);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_signal_connect(combo, "changed", G_CALLBACK(on_live_combo), GINT_TO_POINTER(dim));
    gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

// (Re)construit les boutons d'annotation à partir de l'inventaire du bateau courant.
static void rebuild_live_controls(void) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(L.controls_box));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    L.cur_main[0] = L.cur_head[0] = L.cur_sea[0] = 0;
    L.moteur = FALSE;

    AppWidgets *app = L.app;
    if (g_boat_config.n_mainsail == 0 && g_boat_config.n_headsail == 0) {
        gtk_box_pack_start(GTK_BOX(L.controls_box),
            gtk_label_new(TR(app, "Ouvrir un bateau avec un inventaire pour annoter en live.",
                                  "Open a boat with an inventory to annotate live.")), FALSE, FALSE, 0);
        gtk_widget_show_all(L.controls_box);
        return;
    }
    build_radio_row(L.controls_box, TR(app, "Grand-voile :", "Mainsail:"),
                    g_boat_config.mainsail, g_boat_config.n_mainsail, FALSE, 0);
    build_radio_row(L.controls_box, TR(app, "Voile d'avant :", "Headsail:"),
                    g_boat_config.headsail, g_boat_config.n_headsail, FALSE, 1);
    build_radio_row(L.controls_box, TR(app, "État de mer :", "Sea state:"),
                    g_boat_config.seastate, g_boat_config.n_seastate, TRUE, 2);

    GtkWidget *mot = gtk_toggle_button_new_with_label(TR(app, "⚙ Moteur (exclut les points)",
                                                            "⚙ Engine (excludes points)"));
    g_signal_connect(mot, "toggled", G_CALLBACK(on_live_moteur), NULL);
    gtk_box_pack_start(GTK_BOX(L.controls_box), mot, FALSE, FALSE, 4);
    gtk_widget_show_all(L.controls_box);
}

// --- ingestion ----------------------------------------------------------------

static void live_ingest(double twa, double tws, double stw, gboolean has_sog, double sog) {
    if (has_sog && !stw_sog_accept(&L.stwf, stw, sog)) return;  // débruitage loch
    L.last_twa = twa; L.last_tws = tws; L.last_stw = stw; L.last_sog = has_sog ? sog : 0;
    if (L.moteur) return;                                       // moteur embrayé -> exclu
    for (int k = 0; k < L.n; k++)
        if (polar_def_matches(&g_boat_config.polars[k], L.cur_main, L.cur_head, L.cur_sea))
            add_data_point(&L.grids[k], twa, tws, stw);
    L.points++;
}

// Index de la polaire affichée (sélection du panneau, sinon 0).
static int live_displayed_index(void) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(L.app->polar_list));
    if (row) {
        const char *name = g_object_get_data(G_OBJECT(row), "name");
        for (int k = 0; k < L.n; k++)
            if (name && g_strcmp0(name, g_boat_config.polars[k].name) == 0) return k;
    }
    return 0;
}

static void live_refresh_display(void) {
    int idx = live_displayed_index();
    if (idx < 0 || idx >= L.n) return;
    g_live_grid = &L.grids[idx];          // nuage de points superposé sur le diagramme
    g_live_cur_twa = L.last_twa;
    g_live_cur_bsp = L.last_stw;
    double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS];
    compute_polar(&L.grids[idx], polar, NULL);
    load_polar_from_grid(L.app->polar_data, &L.grids[idx], polar);
    rebuild_data_tab(L.app);

    // Quand de nouvelles colonnes TWS apparaissent (ou qu'on change de polaire),
    // recouvrir TOUTE la plage TWS pour ne pas masquer les hauts vents enregistrés.
    static int prev_speeds = -1, prev_idx = -1;
    if (L.app->polar_data->num_speeds != prev_speeds || idx != prev_idx) {
        prev_speeds = L.app->polar_data->num_speeds;
        prev_idx = idx;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(L.app->tws_from_combo));
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(L.app->tws_to_combo));
        int cnt = 0;
        for (int i = 0; i < L.app->polar_data->num_speeds; i++) {
            int tws = L.app->polar_data->tws_values[i];
            if (tws == 0) continue;
            char t[16]; snprintf(t, sizeof(t), "%d kn", tws);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(L.app->tws_from_combo), t);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(L.app->tws_to_combo), t);
            cnt++;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(L.app->tws_from_combo), 0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(L.app->tws_to_combo), cnt - 1);  // tout afficher
    }

    rebuild_vmg_table(L.app);
    gtk_widget_queue_draw(L.app->polar_view);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "● Live — %d pts | TWA %.0f° TWS %.1f STW %.1f SOG %.1f | %s/%s/%s%s",
             L.points, L.last_twa, L.last_tws, L.last_stw, L.last_sog,
             L.cur_main[0] ? L.cur_main : "?", L.cur_head[0] ? L.cur_head : "?",
             L.cur_sea[0] ? L.cur_sea : "?", L.moteur ? " [MOTEUR]" : "");
    gtk_label_set_text(GTK_LABEL(L.readout), msg);
}

static gboolean live_tick(gpointer unused) {
    (void)unused;
    if (!L.active || !L.db) return G_SOURCE_REMOVE;
    sqlite3_stmt *st;
    const char *q = "SELECT TWA,TWS,STW,SOG,TIME FROM VDR WHERE TIME > ? "
                    "AND TWA IS NOT NULL AND TWS IS NOT NULL AND STW IS NOT NULL AND STW > 0 ORDER BY TIME;";
    if (sqlite3_prepare_v2(L.db, q, -1, &st, NULL) != SQLITE_OK) return G_SOURCE_CONTINUE;
    sqlite3_bind_int64(st, 1, L.last_time);
    int got = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        double twa = fabs(sqlite3_column_double(st, 0));
        double tws = sqlite3_column_double(st, 1);
        double stw = sqlite3_column_double(st, 2);
        gboolean has_sog = (sqlite3_column_type(st, 3) != SQLITE_NULL);
        double sog = has_sog ? sqlite3_column_double(st, 3) : 0;
        L.last_time = (long)sqlite3_column_int64(st, 4);
        if (twa <= 180 && tws >= 0.1 && tws <= 70 && stw >= 0.1 && stw <= 50) {
            live_ingest(twa, tws, stw, has_sog, sog);
            got++;
        }
    }
    sqlite3_finalize(st);
    if (got) live_refresh_display();
    return G_SOURCE_CONTINUE;
}

// --- start / stop -------------------------------------------------------------

static void live_save_all(void) {
    char *folder = g_path_get_dirname(g_boat_config_path);
    for (int k = 0; k < L.n; k++) {
        if (L.grids[k].point_count <= 0) continue;
        double polar[PG_MAX_ANGLES][PG_MAX_SPEEDS];
        compute_polar(&L.grids[k], polar, NULL);
        PolarData pd; init_polar_data(&pd);
        load_polar_from_grid(&pd, &L.grids[k], polar);
        char *fn = g_strdup_printf("%s.pol", g_boat_config.polars[k].name);
        char *path = g_build_filename(folder, fn, NULL);
        g_strlcpy(pd.filename, path, sizeof(pd.filename));
        save_polar_file(path, &pd);
        g_free(fn); g_free(path);
    }
    g_free(folder);
}

static void live_stop(void) {
    if (!L.active) return;
    L.active = FALSE;
    if (L.timer_id) { g_source_remove(L.timer_id); L.timer_id = 0; }
    g_live_grid = NULL; g_live_cur_bsp = 0;   // retire le nuage live du diagramme
    gtk_widget_queue_draw(L.app->polar_view);

    if (L.source == 1) {              // NMEA TCP
        if (L.cancel) g_cancellable_cancel(L.cancel);
        if (L.conn) g_io_stream_close(G_IO_STREAM(L.conn), NULL, NULL);
        if (L.din)  { g_object_unref(L.din);  L.din = NULL; }
        if (L.conn) { g_object_unref(L.conn); L.conn = NULL; }
        if (L.client) { g_object_unref(L.client); L.client = NULL; }
        if (L.cancel) { g_object_unref(L.cancel); L.cancel = NULL; }
    } else if (L.source == 2) {       // NMEA UDP
        if (L.net_source) { g_source_remove(L.net_source); L.net_source = 0; }
        if (L.cancel) { g_cancellable_cancel(L.cancel); g_object_unref(L.cancel); L.cancel = NULL; }
        if (L.usock) { g_socket_close(L.usock, NULL); g_object_unref(L.usock); L.usock = NULL; }
    } else {                          // VDR fichier
        if (L.db) { sqlite3_close(L.db); L.db = NULL; }
    }

    live_save_all();
    for (int k = 0; k < L.n; k++) free_polar_grid(&L.grids[k]);
    g_free(L.grids); L.grids = NULL; L.n = 0;
    gtk_button_set_label(GTK_BUTTON(L.toggle), TR(L.app, "⏺ Démarrer", "⏺ Start"));
    gtk_label_set_text(GTK_LABEL(L.readout), TR(L.app, "Arrêté (polaires enregistrées).", "Stopped (polars saved)."));
}

// Construit les grilles (une par polaire), ré-ensemencées depuis les .pol existants.
static void live_setup_grids(void) {
    L.n = g_boat_config.n_polars;
    L.grids = g_new0(polar_grid_t, L.n);
    char *folder = g_path_get_dirname(g_boat_config_path);
    for (int k = 0; k < L.n; k++) {
        init_polar_grid(&L.grids[k]);
        char *fn = g_strdup_printf("%s.pol", g_boat_config.polars[k].name);
        char *path = g_build_filename(folder, fn, NULL);
        PolarData pd; init_polar_data(&pd);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR) && load_polar_file(path, &pd))
            load_polar_from_memory(&pd, &L.grids[k]);  // ré-injecte les points existants
        g_free(fn); g_free(path);
    }
    g_free(folder);
    stw_sog_reset(&L.stwf);
    L.points = 0;
}

// Source NMEA : l'ingestion se fait au fil du socket ; on rafraîchit l'affichage périodiquement.
static gboolean live_refresh_tick(gpointer u) {
    (void)u;
    if (L.active) live_refresh_display();
    return G_SOURCE_CONTINUE;
}

// Parse une trame et, si un point est complet, l'ingère (état des boutons).
static void live_feed_line(const char *line) {
    if (!parse_nmea_sentence(line, &L.nmea)) return;
    double twa = L.nmea.twa, tws = L.nmea.tws, bsp = L.nmea.bsp;
    if (NMEA_SMOOTH_WINDOW > 1)
        nmea_smoother_push(&L.smoother, L.nmea.twa, L.nmea.tws, L.nmea.bsp, &twa, &tws, &bsp);
    live_ingest(twa, tws, bsp, L.nmea.has_sog, L.nmea.sog);
}

// NMEA TCP : réception asynchrone ligne à ligne (réenchaînée tant que ça tourne).
static void on_nmea_line(GObject *src, GAsyncResult *res, gpointer u) {
    (void)u;
    gsize len = 0; GError *e = NULL;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &e);
    if (e) { g_error_free(e); return; }   // annulé/erreur
    if (!line) return;                    // EOF (flux fermé)
    live_feed_line(line);
    g_free(line);
    if (L.active)
        g_data_input_stream_read_line_async(L.din, G_PRIORITY_DEFAULT, L.cancel, on_nmea_line, NULL);
}

// NMEA UDP : un datagramme peut porter plusieurs trames -> découpage en lignes.
static gboolean on_udp_data(GSocket *sock, GIOCondition cond, gpointer u) {
    (void)cond; (void)u;
    if (!L.active) return G_SOURCE_REMOVE;
    char buf[2048]; GError *e = NULL;
    gssize n = g_socket_receive(sock, buf, sizeof(buf) - 1, NULL, &e);
    if (n <= 0) { if (e) g_error_free(e); return G_SOURCE_CONTINUE; }
    buf[n] = 0;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\r\n", &save); ln; ln = strtok_r(NULL, "\r\n", &save))
        live_feed_line(ln);
    return G_SOURCE_CONTINUE;
}

// Extrait hôte + port d'une saisie « hôte:port » (ou « port » seul pour l'UDP).
static void live_parse_hostport(char *host, size_t hsz, int *port, int defport) {
    const char *hp = gtk_entry_get_text(GTK_ENTRY(L.net_entry));
    g_strlcpy(host, "localhost", hsz);
    *port = defport;
    const char *colon = strrchr(hp, ':');
    if (colon) {
        int hl = (int)(colon - hp);
        if (hl > 0 && hl < (int)hsz) { memcpy(host, hp, hl); host[hl] = 0; }
        *port = atoi(colon + 1);
    } else if (*hp) {
        if (g_ascii_isdigit(*hp)) *port = atoi(hp);   // « port » seul
        else g_strlcpy(host, hp, hsz);
    }
    if (*port <= 0) *port = defport;
}

static gboolean live_start_vdr(void) {
    g_strlcpy(L.db_path, gtk_entry_get_text(GTK_ENTRY(L.path_entry)), sizeof(L.db_path));
    if (sqlite3_open_v2(L.db_path, &L.db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(L.app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s : %s",
            TR(L.app, "Impossible d'ouvrir le VDR", "Cannot open the VDR"), L.db_path);
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        if (L.db) { sqlite3_close(L.db); L.db = NULL; }
        return FALSE;
    }
    sqlite3_stmt *st;
    L.last_time = 0;
    if (sqlite3_prepare_v2(L.db, "SELECT MAX(TIME) FROM VDR;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) L.last_time = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    L.timer_id = g_timeout_add(LIVE_INTERVAL_MS, live_tick, NULL);
    return TRUE;
}

static gboolean live_start_nmea_tcp(void) {
    char host[128]; int port;
    live_parse_hostport(host, sizeof(host), &port, 10110);
    L.client = g_socket_client_new();
    GError *e = NULL;
    L.conn = g_socket_client_connect_to_host(L.client, host, port, NULL, &e);
    if (!L.conn) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(L.app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s %s:%d\n%s",
            TR(L.app, "Connexion NMEA TCP impossible :", "Cannot connect to NMEA TCP:"),
            host, port, e ? e->message : "");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        if (e) g_error_free(e);
        g_object_unref(L.client); L.client = NULL;
        return FALSE;
    }
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(L.conn));
    L.din = g_data_input_stream_new(in);
    L.cancel = g_cancellable_new();
    memset(&L.nmea, 0, sizeof(L.nmea));
    nmea_smoother_reset(&L.smoother);
    g_data_input_stream_read_line_async(L.din, G_PRIORITY_DEFAULT, L.cancel, on_nmea_line, NULL);
    L.timer_id = g_timeout_add(1500, live_refresh_tick, NULL);  // rafraîchit l'affichage
    return TRUE;
}

static gboolean live_start_nmea_udp(void) {
    char host[128]; int port;
    live_parse_hostport(host, sizeof(host), &port, 10110);  // hôte ignoré (on écoute)
    GError *e = NULL;
    L.usock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &e);
    GInetAddress *any = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *addr = g_inet_socket_address_new(any, port);
    gboolean ok = L.usock && g_socket_bind(L.usock, addr, TRUE, &e);  // reuse-addr
    g_object_unref(any); g_object_unref(addr);
    if (!ok) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(L.app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s %d\n%s",
            TR(L.app, "Écoute UDP impossible sur le port", "Cannot listen UDP on port"),
            port, e ? e->message : "");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        if (e) g_error_free(e);
        if (L.usock) { g_object_unref(L.usock); L.usock = NULL; }
        return FALSE;
    }
    g_socket_set_broadcast(L.usock, TRUE);
    memset(&L.nmea, 0, sizeof(L.nmea));
    nmea_smoother_reset(&L.smoother);
    L.cancel = g_cancellable_new();
    GSource *s = g_socket_create_source(L.usock, G_IO_IN, L.cancel);
    g_source_set_callback(s, (GSourceFunc)on_udp_data, NULL, NULL);
    L.net_source = g_source_attach(s, NULL);
    g_source_unref(s);
    L.timer_id = g_timeout_add(1500, live_refresh_tick, NULL);
    return TRUE;
}

static void live_start(void) {
    AppWidgets *app = L.app;
    if (!(g_boat_config.n_polars > 0 && g_boat_config_path[0])) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s",
            TR(app, "Ouvre un bateau et définis ses polaires avant la capture live.",
                   "Open a boat and define its polars before live capture."));
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        return;
    }
    L.source = gtk_combo_box_get_active(GTK_COMBO_BOX(L.source_combo));
    live_setup_grids();
    gboolean ok = (L.source == 1) ? live_start_nmea_tcp()
                : (L.source == 2) ? live_start_nmea_udp() : live_start_vdr();
    if (!ok) {
        for (int k = 0; k < L.n; k++) free_polar_grid(&L.grids[k]);
        g_free(L.grids); L.grids = NULL; L.n = 0;
        return;
    }
    rebuild_live_controls();
    L.active = TRUE;
    gtk_button_set_label(GTK_BUTTON(L.toggle), TR(app, "⏹ Arrêter", "⏹ Stop"));
    gtk_label_set_text(GTK_LABEL(L.readout), TR(app, "● Live démarré…", "● Live started…"));
}

static void on_live_toggle(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (L.active) live_stop(); else live_start();
}

// --- onglet -------------------------------------------------------------------

GtkWidget *create_live_tab(AppWidgets *app) {
    memset(&L, 0, sizeof(L));
    L.app = app;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_widget_set_size_request(box, 250, -1);  // colonne étroite à droite du diagramme

    GtkWidget *title = gtk_label_new(TR(app, "Capture live", "Live capture"));
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    PangoAttrList *tb = pango_attr_list_new();
    pango_attr_list_insert(tb, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title), tb);
    pango_attr_list_unref(tb);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    // Choix de la source
    L.source_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(L.source_combo), TR(app, "VDR qtVlm (fichier)", "qtVlm VDR (file)"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(L.source_combo), TR(app, "NMEA réseau (TCP)", "NMEA network (TCP)"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(L.source_combo), TR(app, "NMEA réseau (UDP)", "NMEA network (UDP)"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(L.source_combo), 0);
    gtk_box_pack_start(GTK_BOX(box), L.source_combo, FALSE, FALSE, 0);

    GtkWidget *plabel = gtk_label_new(TR(app, "Fichier VDR :", "VDR file:"));
    gtk_label_set_xalign(GTK_LABEL(plabel), 0.0);
    gtk_box_pack_start(GTK_BOX(box), plabel, FALSE, FALSE, 0);
    L.path_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(L.path_entry), 12);  // ne dicte pas la largeur
    char *def = g_build_filename(g_get_home_dir(), ".qtVlm", "vdrs", "vdr.db", NULL);
    gtk_entry_set_text(GTK_ENTRY(L.path_entry), def);
    g_free(def);
    gtk_box_pack_start(GTK_BOX(box), L.path_entry, FALSE, FALSE, 0);

    GtkWidget *nlabel = gtk_label_new(TR(app, "NMEA hôte:port (UDP = port) :", "NMEA host:port (UDP = port):"));
    gtk_label_set_xalign(GTK_LABEL(nlabel), 0.0);
    gtk_box_pack_start(GTK_BOX(box), nlabel, FALSE, FALSE, 0);
    L.net_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(L.net_entry), 12);
    gtk_entry_set_text(GTK_ENTRY(L.net_entry), "localhost:10110");
    gtk_box_pack_start(GTK_BOX(box), L.net_entry, FALSE, FALSE, 0);

    L.toggle = gtk_button_new_with_label(TR(app, "⏺ Démarrer", "⏺ Start"));
    g_signal_connect(L.toggle, "clicked", G_CALLBACK(on_live_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(box), L.toggle, FALSE, FALSE, 0);

    L.readout = gtk_label_new(TR(app, "Arrêté.", "Stopped."));
    gtk_label_set_xalign(GTK_LABEL(L.readout), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(L.readout), TRUE);
    gtk_box_pack_start(GTK_BOX(box), L.readout, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    L.controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(box), L.controls_box, FALSE, FALSE, 0);
    rebuild_live_controls();

    return box;
}

// Rafraîchit les boutons d'annotation quand un bateau est ouvert.
void live_inventory_changed(void) {
    if (L.controls_box && !L.active) rebuild_live_controls();
}
