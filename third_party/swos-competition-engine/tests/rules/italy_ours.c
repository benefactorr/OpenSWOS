/* The library's sr_italy over the same random final tables (tables.h): Italy 2026-27's 14
 * divisions, Serie B = division 1, Serie C's three groups 2..4. Each season's moves. */
#include <stdio.h>
#include <stdlib.h>
#include "tables.h"
#include "swosrules.h"
static const uint8_t PROM[16] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t REL[16] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static sr_state st;
int main(int argc, char **argv)
{
    unsigned N = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    struct trow t[16][32]; int n[16];
    const sr_league lg = { 14, PROM, REL, 1, 1, 2, 3 };
    sr_reset(&st);
    for (unsigned it = 0; it < N; it++) {
        sr_season se = { 0 };
        make_tables(it, t, n);
        se.cup_winner = cup_winner(it, t, n);
        st.t.nd = (uint8_t)ND;
        for (int d = 0; d < ND; d++) {
            st.t.n[d] = (uint8_t)n[d];
            for (int k = 0; k < n[d]; k++) {
                sr_row *r = &st.t.t[d][k];
                r->team = t[d][k].team; r->glob = t[d][k].glob; r->won = t[d][k].won;
                r->gf = t[d][k].gf; r->ga = t[d][k].ga; r->pts = t[d][k].pts;
            }
        }
        sr_italy(&st, &lg, &se);
        printf("%u", it);
        for (int i = 0; i < st.nmv; i++) printf(" d%u:%d", st.mv[i].glob, st.mv[i].delta);
        printf("\n");
    }
    return 0;
}
