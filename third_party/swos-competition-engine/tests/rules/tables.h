/* league_playoffs check: random final tables for a league of 14 divisions (Italy 2026-27's
 * sizes; or another league's, given as "20,24,24,24": the pack's knockouts only), the same
 * in both programs (their own LCG, not rand()). */
static int SIZES[16] = { 20, 20, 20, 20, 20, 17, 17, 17, 18, 17, 18, 17, 17, 18 };
static int ND = 14, GP_ONLY;
static void set_shape(const char *s)
{
    ND = 0;
    GP_ONLY = 1;
    while (*s && ND < 16) {
        SIZES[ND++] = atoi(s);
        while (*s && *s != ',') s++;
        if (*s) s++;
    }
}
static unsigned tab_seed;
static unsigned tab_rand(unsigned n) { tab_seed = tab_seed * 1664525u + 1013904223u; return (tab_seed >> 8) % n; }
struct trow { unsigned short team, glob, won, gf, ga, pts; };
static void make_tables(unsigned it, struct trow t[16][32], int n[16])
{
    tab_seed = it * 2654435761u + 1;
    for (int d = 0; d < ND; d++) {
        int pts[32];
        n[d] = SIZES[d];
        for (int k = 0; k < n[d]; k++) pts[k] = 20 + tab_rand(60);
        for (int i = 0; i < n[d]; i++) for (int j = i + 1; j < n[d]; j++)
            if (pts[j] > pts[i]) { int x = pts[i]; pts[i] = pts[j]; pts[j] = x; }
        for (int k = 0; k < n[d]; k++) {
            struct trow *r = &t[d][k];
            r->team = (unsigned short)(0x1400 + d * 32 + k); r->glob = (unsigned short)(424 + d * 20 + k);
            r->pts = pts[k]; r->won = pts[k] / 3; r->gf = 30 + tab_rand(30); r->ga = 40;
        }
    }
}
/* B teams (built with -DKO_BT): two B teams -- globals 444, 445, team words 1420h, 1421h, the
 * second division's first two rows -- and their first teams (424, 425: the top division's), each
 * season at random places of the tables, a B team in a division below its first team's; the
 * rows' places, points and records stay where they were (their identities move). Both programs
 * the same (their own hash). */
#ifdef KO_BT
static const unsigned short BT_B[2] = { 444, 445 }, BT_F[2] = { 424, 425 };
static void bt_swap(struct trow t[16][32], int d1, int k1, int d2, int k2)
{
    struct trow *a = &t[d1][k1], *b = &t[d2][k2];
    unsigned short x = a->team, y = a->glob;
    a->team = b->team; a->glob = b->glob; b->team = x; b->glob = y;
}
static void bt_move(struct trow t[16][32], const int n[16], unsigned short g, int d, int k)
{
    for (int e = 0; e < ND; e++)
        for (int j = 0; j < n[e]; j++)
            if (t[e][j].glob == g) { bt_swap(t, e, j, d, k); return; }
}
static void bt_place(unsigned it, struct trow t[16][32], const int n[16])
{
    unsigned h = it * 2654435761u + 31u;
    if (ND < 2)
        return;
    for (int i = 0; i < 2; i++) {
        int df, db;
        h = h * 1664525u + 1013904223u; df = (int)((h >> 8) % (unsigned)(ND - 1));
        h = h * 1664525u + 1013904223u; db = df + 1 + (int)((h >> 8) % (unsigned)(ND - 1 - df));
        h = h * 1664525u + 1013904223u; bt_move(t, n, BT_F[i], df, (int)((h >> 8) % (unsigned)n[df]));
        h = h * 1664525u + 1013904223u; bt_move(t, n, BT_B[i], db, (int)((h >> 8) % (unsigned)n[db]));
    }
}
#endif

/* The season's stage tables (the kept 'keep' table and the four GP_SS ones) of the top
 * division: its clubs in a random order with random points, each table written in about
 * three seasons of four -- its own hash, so the tables above stay the ones they were. */
struct stages { int n[4], fresh, nkeep; struct trow r[4][32], keep[32]; };
static void make_stages(unsigned it, struct trow t[16][32], const int n[16], struct stages *s)
{
    unsigned h = it * 2246822519u + 7u;
    s->fresh = 0;
    s->nkeep = 0;
    for (int k = 0; k < 5; k++) {
        int m = n[0], ord[32];
        struct trow *o = k < 4 ? s->r[k] : s->keep;
        h = h * 1664525u + 1013904223u;
        if ((h >> 24) % 4 == 0) { if (k < 4) s->n[k] = 0; continue; }
        for (int i = 0; i < m; i++) ord[i] = i;
        for (int i = m - 1; i > 0; i--) {
            h = h * 1664525u + 1013904223u;
            int j = (int)((h >> 8) % (unsigned)(i + 1)), x = ord[i]; ord[i] = ord[j]; ord[j] = x;
        }
        for (int i = 0; i < m; i++) {
            o[i] = t[0][ord[i]];
            o[i].pts = (unsigned short)(40 - i);
        }
        if (k < 4) { s->n[k] = m; s->fresh |= 1 << k; }
        else s->nkeep = m < 21 ? m : 21;
    }
}

/* The season's Coppa Italia Serie C winner (dplay.c's cupw): none a quarter of the time,
 * else any club of Serie C's groups (divisions 2..4), at any place -- its own hash, so the
 * tables stay the ones before it was added. */
static unsigned short cup_winner(unsigned it, struct trow t[16][32], const int n[16])
{
    unsigned h = it * 2654435761u + 97u;
    h ^= h >> 15;
    if (h % 4 == 0 || GP_ONLY)
        return 0;
    int d = 2 + (int)((h >> 4) % 3);
    return t[d][(h >> 8) % (unsigned)n[d]].team;
}

static unsigned char spec[4096];
static unsigned spec_size;              /* (its bytes: sr_league.spec_len) */
static void load_spec(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f || (spec_size = (unsigned)fread(spec, 1, sizeof spec, f)) == 0) { fprintf(stderr, "no spec %s\n", path); exit(2); }
    fclose(f);
}
