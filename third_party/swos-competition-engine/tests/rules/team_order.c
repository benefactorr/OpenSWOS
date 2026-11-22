/* sr_league.team_file_high: a spec names a club by its (file, ordinal) bytes (entrant type 8), each
 * game lays its team words out its own way -- the Mod's file | ordinal << 8, OpenSWOS's file << 8 |
 * ordinal. The same seasons played both ways (the tables' team words in that game's layout, the
 * switch to match) must decide the same: every move, every result. And a layout the switch does not
 * describe must find neither club (they are absent: the play-off is not played). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tables.h"
#include "swosrules.h"

static const uint8_t PROM[16] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t REL[16] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
enum { FILE_ = 0x14, A = 32 + 3, B = 32 + 5 };     /* (two clubs of the second division, by ordinal) */
/* a promotion play-off: the two clubs (type 8, division 1), one tie, the winner to division 0 */
static const uint8_t SPEC[] = {
    1, 2, 1, 0, 1, 0,
    8, 1, FILE_, A,  8, 1, FILE_, B,
    1, 1, 0, 1, 0, 0,  0, 1,
    0
};
static sr_state st;

static uint16_t word(int high, int ordinal)
{
    return high ? (uint16_t)(FILE_ << 8 | ordinal) : (uint16_t)(FILE_ | ordinal << 8);
}

/* play-off 0's entrants from the tables of `it`, the words laid out as `rows`, the rules told
   `flag`: their (file, ordinal) pairs in out[], their count (0: one not found) */
static int entrants(unsigned it, int rows, int flag, int out[2][2])
{
    struct trow t[16][32]; int n[16];
    static sr_row tab[32];
    sr_league lg = { 2, PROM, REL, 0, 0, 0, 0, SPEC, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    sr_season se;
    uint16_t team[64], glob[64], pts[64];
    uint8_t prio[64];
    const uint8_t *sp;
    memset(&se, 0, sizeof se);
    lg.spec_len = sizeof SPEC;
    lg.team_file_high = (uint8_t)flag;
    make_tables(it, t, n);
    st.t.nd = 2;
    for (int d = 0; d < 2; d++) {
        st.t.n[d] = (uint8_t)n[d];
        for (int k = 0; k < n[d]; k++) {
            sr_row *r = &st.t.t[d][k];
            r->team = word(rows, d * 32 + k); r->glob = t[d][k].glob; r->won = t[d][k].won;
            r->gf = t[d][k].gf; r->ga = t[d][k].ga; r->pts = t[d][k].pts;
            if (d == 1) tab[k] = *r;
        }
    }
    {
        const sr_row *tabs[1] = { tab };
        int32_t divs[1] = { 1 }, ns[1] = { n[1] };
        se.mgr = word(rows, A);
        const int32_t ne = sr_contest_entrants(&st, &lg, &se, 1, divs, tabs, ns, 0, team, glob, prio, pts, &sp);
        if (ne != 2) return 0;
    }
    for (int i = 0; i < 2; i++) {             /* (back to (file, ordinal), from this layout) */
        out[i][0] = rows ? team[i] >> 8 : team[i] & 0xFF;
        out[i][1] = rows ? team[i] & 0xFF : team[i] >> 8;
    }
    return 2;
}

int main(void)
{
    enum { N = 2000 };
    int bad = 0;
    ND = 2;
    sr_reset(&st);
    for (unsigned it = 0; it < N && bad < 4; it++) {
        int m[2][2], o[2][2], w[2][2];
        if (entrants(it, 0, 0, m) != 2 || entrants(it, 1, 1, o) != 2) {
            printf("FAIL: season %u: a club not found\n", it); bad++; continue;
        }
        if (m[0][0] != FILE_ || m[0][1] != A || m[1][0] != FILE_ || m[1][1] != B || memcmp(m, o, sizeof m)) {
            printf("FAIL: season %u: the Mod's layout (%02X,%d) (%02X,%d), OpenSWOS's (%02X,%d) (%02X,%d)\n", it,
                   m[0][0], m[0][1], m[1][0], m[1][1], o[0][0], o[0][1], o[1][0], o[1][1]); bad++;
        }
        if (entrants(it, 1, 0, w) == 2) { printf("FAIL: season %u: file << 8 | ordinal words, the switch 0: found\n", it); bad++; }
    }
    printf("%s: a club by (file, ordinal), %d seasons: the same two clubs from team words in either layout"
           " (team_file_high 0 / 1); a layout the switch does not describe finds neither\n", bad ? "FAIL" : "PASS", N);
    return bad != 0;
}
