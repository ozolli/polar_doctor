/* test_n2k.c — décodeur NMEA 2000 YDRAW (import.c). Lancer : make test */
#include "libpolar.h"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("ÉCHEC l.%d : ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, e) (fabs((a) - (b)) < (e))
/* Ligne YDRAW : PGN diffusé (PDU2), priorité 2, source 35 */
static void yd(char *out, int pgn, const uint8_t *b, int n) {
    unsigned id = (2u << 26) | ((unsigned)pgn << 8) | 35u;
    int o = sprintf(out, "12:00:00.000 R %08X", id);
    for (int i = 0; i < n; i++) o += sprintf(out + o, " %02X", b[i]);
}
static void wind(char *out, double ms, double deg, int ref) {
    unsigned v = (unsigned)lround(ms / 0.01), a = (unsigned)lround(deg * M_PI / 180 / 1e-4);
    uint8_t b[8] = {0xff, v & 0xff, v >> 8, a & 0xff, a >> 8, (uint8_t)(0xf8 | ref), 0xff, 0xff};
    yd(out, 130306, b, 8);
}
static void stw(char *out, double ms) {
    unsigned v = (unsigned)lround(ms / 0.01);
    uint8_t b[8] = {0xff, v & 0xff, v >> 8, 0xff, 0xff, 0, 0xff, 0xff};
    yd(out, 128259, b, 8);
}
static void sog(char *out, double ms) {
    unsigned v = (unsigned)lround(ms / 0.01);
    uint8_t b[8] = {0xff, 0xfc, 0, 0, v & 0xff, v >> 8, 0xff, 0xff};
    yd(out, 129026, b, 8);
}
static void hdg(char *out, double deg, double var, int ref) {
    unsigned a = (unsigned)lround(deg * M_PI / 180 / 1e-4);
    int16_t w = var > 900 ? 0x7FFF : (int16_t)lround(var * M_PI / 180 / 1e-4);
    uint8_t b[8] = {0xff, a & 0xff, a >> 8, 0, 0, (uint8_t)(w & 0xff), (uint8_t)((uint16_t)w >> 8), (uint8_t)(0xfc | ref)};
    yd(out, 127250, b, 8);
}
int main(void) {
    /* 0. fichier YDRAW du simulateur n2k-mux : 53 points */
    { static polar_grid_t g; init_polar_grid(&g);
      int n = process_file("Test/n2k-mux-sim.ydraw", &g, NULL);
      CHECK(n == 53, "Test/n2k-mux-sim.ydraw : %d points (53 attendus)", n); }

    char l[128]; nmea_data_t d;
    CHECK(n2k_pgn_from_id(0x09FD0223) == 130306, "pgn vent");
    CHECK(n2k_pgn_from_id(0x09F50323) == 128259, "pgn speed");
    CHECK(n2k_pgn_from_id(0x09F80223) == 129026, "pgn cogsog");
    CHECK(n2k_pgn_from_id(0x09F11223) == 127250, "pgn heading");
    CHECK(n2k_pgn_from_id(0x18EAFF01) == 59904, "pgn PDU1 (destination hors PGN)");

    /* 1. vent vrai/eau bâbord 315° 10 m/s + STW 3 m/s -> point TWA 45 */
    memset(&d, 0, sizeof d);
    wind(l, 10, 315, 4); CHECK(!parse_nav_line(l, &d), "vent seul ne complète pas");
    CHECK(NEAR(d.twa, 45, 0.01) && NEAR(d.tws, 19.438, 0.01), "twa=%.2f tws=%.2f", d.twa, d.tws);
    stw(l, 3); CHECK(parse_nav_line(l, &d), "STW complète le point");
    CHECK(NEAR(d.bsp, 5.83, 0.01), "bsp=%.3f", d.bsp);
    /* 2. apparent ignoré */
    wind(l, 20, 30, 2); parse_nav_line(l, &d);
    CHECK(NEAR(d.twa, 45, 0.01) && NEAR(d.tws, 19.438, 0.01), "apparent a écrasé : twa=%.2f", d.twa);
    /* 3. vent fond (réf 0) ignoré une fois le vent eau vu */
    wind(l, 5, 200, 0); parse_nav_line(l, &d);
    CHECK(NEAR(d.twa, 45, 0.01), "vent fond a écrasé le vent eau");
    /* 4. SOG : pas de point, has_sog */
    sog(l, 3.1); CHECK(!parse_nav_line(l, &d) && d.has_sog && NEAR(d.sog, 6.03, 0.01), "sog=%.2f", d.sog);

    /* 5. sans vent eau : direction 100° + cap vrai 40° -> TWA 60 */
    memset(&d, 0, sizeof d);
    wind(l, 8, 100, 0); parse_nav_line(l, &d);
    stw(l, 3); parse_nav_line(l, &d);
    CHECK(!d.has_twa, "pas de TWA sans cap");
    hdg(l, 40, 0, 0); CHECK(parse_nav_line(l, &d), "cap complète le point");
    CHECK(NEAR(d.twa, 60, 0.05), "twa=%.2f", d.twa);
    /* 6. cap magnétique 45° + variation −5° -> vrai 40° -> TWA 60 */
    hdg(l, 45, -5, 1); parse_nav_line(l, &d);
    CHECK(NEAR(d.heading, 40, 0.05), "heading=%.2f", d.heading);
    /* 7. cap magnétique sans variation : ignoré */
    hdg(l, 90, 999, 1); parse_nav_line(l, &d);
    CHECK(NEAR(d.heading, 40, 0.05), "cap mag sans variation accepté : %.2f", d.heading);
    /* 8. vrai/bateau (réf 3), angle à l'étrave */
    memset(&d, 0, sizeof d);
    wind(l, 6, 120, 3); parse_nav_line(l, &d);
    CHECK(NEAR(d.twa, 120, 0.05) && !d.has_mwv_true, "ref3 twa=%.2f", d.twa);

    /* 9. valeurs non disponibles et lignes invalides */
    memset(&d, 0, sizeof d);
    CHECK(!parse_nav_line("12:00:00.000 R 09FD0223 FF FF FF 00 00 FC FF FF", &d) && !d.has_tws, "vitesse n/a acceptée");
    CHECK(!parse_nav_line("12:00:00.000 R 09F50323 FF FF FF", &d) && !d.has_bsp, "STW n/a acceptée");
    CHECK(!parse_nav_line("12:00:00.000 X 09F50323 FF 2C 01", &d) && !d.has_bsp, "direction X acceptée");
    CHECK(!parse_nav_line("12:00:00.000 R 3FFFFFFFF 00", &d), "id > 29 bits accepté");
    CHECK(!parse_nav_line("12:00:00.000 R 09F50323 FF 2C1 01", &d) && !d.has_bsp, "octet à 3 chiffres accepté");
    CHECK(!parse_nav_line("", &d) && !parse_nav_line("   ", &d) && !parse_nav_line("!AIVDM,1,1,,A,x,0*00", &d), "lignes vides/AIS");
    /* trame courte : 128259 sur 3 octets suffit */
    CHECK(!parse_nav_line("12:00:00.000 R 09F50323 FF 2C 01", &d) && d.has_bsp && NEAR(d.bsp, 3.0*1.94384, 0.01), "STW 3 octets bsp=%.2f", d.bsp);

    /* 10. le 0183 passe toujours par parse_nav_line */
    memset(&d, 0, sizeof d);
    parse_nav_line("$IIMWV,315.0,T,12.3,N,A*0C", &d);
    CHECK(d.has_mwv_true && NEAR(d.twa, 45, 0.01), "0183 via parse_nav_line (twa=%.1f, has=%d)", d.twa, d.has_mwv_true);

    printf(fails ? "%d échec(s)\n" : "OK : tous les tests passent\n", fails);
    return fails != 0;
}
