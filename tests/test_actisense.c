/* test_actisense.c — décodeur série Actisense NGT-1/NGX-1 (import.c). Lancer : make test */
#include "libpolar.h"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("ÉCHEC l.%d : ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Encode un message 0x93 comme la passerelle : DLE STX, contenu à DLE doublés, DLE ETX. */
static size_t frame93(uint8_t *out, int pgn, int src, const uint8_t *d, int n, int bad_sum) {
    uint8_t body[300]; size_t b = 0;
    body[b++] = 0x93; body[b++] = (uint8_t)(11 + n);
    body[b++] = 2; body[b++] = pgn & 0xff; body[b++] = (pgn >> 8) & 0xff; body[b++] = (pgn >> 16) & 0xff;
    body[b++] = 255; body[b++] = (uint8_t)src;
    body[b++] = 0x10; body[b++] = 0x20; body[b++] = 0x30; body[b++] = 0x40;   /* horodatage (avec un DLE) */
    body[b++] = (uint8_t)n;
    for (int i = 0; i < n; i++) body[b++] = d[i];
    uint8_t sum = 0; for (size_t i = 0; i < b; i++) sum += body[i];
    body[b++] = (uint8_t)(0x100 - sum + (bad_sum ? 1 : 0));
    size_t o = 0; out[o++] = 0x10; out[o++] = 0x02;
    for (size_t i = 0; i < b; i++) { if (body[i] == 0x10) out[o++] = 0x10; out[o++] = body[i]; }
    out[o++] = 0x10; out[o++] = 0x03;
    return o;
}

/* Pousse un flux, applique chaque message au pipeline N2K ; renvoie le nombre de messages. */
static int feed(actisense_rx_t *r, const uint8_t *s, size_t n, nmea_data_t *d, int *points, int *last_pgn) {
    int msgs = 0;
    for (size_t i = 0; i < n; i++) {
        int pgn, len; const uint8_t *data;
        if (actisense_rx_byte(r, s[i], &pgn, &data, &len)) {
            msgs++; *last_pgn = pgn;
            if (n2k_apply_frame(pgn, data, len, d)) (*points)++;
        }
    }
    return msgs;
}

int main(void) {
    uint8_t st[64]; size_t sl = actisense_startup_frame(st, sizeof st);
    const uint8_t want[] = { 0x10, 0x02, 0xA1, 0x03, 0x11, 0x02, 0x00, 0x49, 0x10, 0x03 };
    CHECK(sl == sizeof want && memcmp(st, want, sl) == 0, "trame de démarrage (canboat : ... 49 DLE ETX)");

    actisense_rx_t r; actisense_rx_reset(&r);
    nmea_data_t d; memset(&d, 0, sizeof d);
    uint8_t s[4096]; size_t n = 0;
    /* bruit avant synchro */
    const uint8_t junk[] = { 0x00, 0x03, 0x10, 0x55, 0xFF, 0x02 }; memcpy(s + n, junk, sizeof junk); n += sizeof junk;
    /* vent vrai/eau : 10,00 m/s (0x03E8), 315° = 5,4978 rad (0xD6C2), réf. 4 */
    const uint8_t w[8] = { 0xff, 0xE8, 0x03, 0xC2, 0xD6, 0xFC, 0xff, 0xff };
    n += frame93(s + n, 130306, 5, w, 8, 0);
    /* STW 4,16 m/s = 0x01A0 ; 0x10 dans les données (octet 3 = 0x10) pour l'échappement */
    const uint8_t v[8] = { 0xff, 0xA0, 0x01, 0x10, 0x10, 0x00, 0xff, 0xff };
    n += frame93(s + n, 128259, 0x10, v, 8, 0);
    int points = 0, last = 0;
    int msgs = feed(&r, s, n, &d, &points, &last);
    CHECK(msgs == 2, "messages décodés : %d (2 attendus)", msgs);
    CHECK(points == 1, "points : %d (1 attendu)", points);
    CHECK(fabs(d.twa - 45.0) < 0.05 && fabs(d.tws - 19.438) < 0.01, "vent twa=%.2f tws=%.3f", d.twa, d.tws);
    CHECK(fabs(d.bsp - 4.16 * 1.9438445) < 0.01, "bsp=%.3f", d.bsp);

    /* somme fausse : ignoré */
    n = frame93(s, 128259, 5, (const uint8_t[]){ 0xff, 0x00, 0x02, 0, 0, 0, 0xff, 0xff }, 8, 1);
    msgs = feed(&r, s, n, &d, &points, &last);
    CHECK(msgs == 0 && fabs(d.bsp - 4.16 * 1.9438445) < 0.01, "somme fausse acceptée");

    /* message long (fast-packet réassemblé, 20 octets) : décodé, PGN transmis */
    uint8_t big[20]; for (int i = 0; i < 20; i++) big[i] = (uint8_t)i;
    n = frame93(s, 129029, 7, big, 20, 0);
    msgs = feed(&r, s, n, &d, &points, &last);
    CHECK(msgs == 1 && last == 129029, "message long : msgs=%d pgn=%d", msgs, last);

    /* trame coupée puis nouvelle trame (DLE STX resynchronise) */
    n = frame93(s, 128259, 5, v, 8, 0);
    size_t half = n / 2;
    n = half + frame93(s + half, 128259, 5, v, 8, 0);
    msgs = feed(&r, s, n, &d, &points, &last);
    CHECK(msgs == 1, "resynchro après trame coupée : %d message(s)", msgs);

    /* autre commande (0xA0, message NGT) : ignorée */
    const uint8_t a0[] = { 0x10, 0x02, 0xA0, 0x01, 0x05, 0x5A, 0x10, 0x03 };
    msgs = feed(&r, a0, sizeof a0, &d, &points, &last);
    CHECK(msgs == 0, "commande 0xA0 acceptée");

    printf(fails ? "%d échec(s)\n" : "OK : tous les tests passent\n", fails);
    return fails != 0;
}
