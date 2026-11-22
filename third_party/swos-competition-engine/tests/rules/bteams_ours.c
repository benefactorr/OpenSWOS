/* The library's B-team rules (sr_final_moves, sr_b_teams) on built cases -- Italy 2026-27's B
 * teams with their first teams placed on purpose -- and, with `random N`, on the random cases of
 * sec_cases.h, each club's move printed as bteams_mod.c prints the Mod's sec_post. */
#include <stdio.h>
#include <stdlib.h>
#include "swosrules.h"
#include "sec_cases.h"
static const uint8_t PROM[14] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t REL[14] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static const uint8_t SEC[3][2] = { { 44, 5 }, { 67, 6 }, { 89, 0 } };   /* Juve NG, Atalanta U23, Inter U23 */
static const uint8_t FIRST[4] = { 0, 1, 2, 5 };                         /* A, B, C's groups, D's groups */
static const sr_levels LV = { 4, FIRST };
static const sr_league LP = { 14, PROM, REL, 1, 1, 2, 3, 0, 0, 0, 3, SEC, 1, 3 };
enum { BASE = 424, JUVE = 5, JUVENG = 44, INTER = 0, INTERU23 = 89 };
static sr_state st;

static void tables(void)                /* every place a club of its own, no B team anywhere */
{
    static const int sz[14] = { 20, 20, 20, 20, 20, 17, 17, 17, 18, 17, 18, 17, 17, 18 };
    st.t.nd = 14;
    st.nmv = 0;
    for (int d = 0; d < 14; d++) {
        st.t.n[d] = (uint8_t)sz[d];
        for (int k = 0; k < sz[d]; k++) st.t.t[d][k].glob = (uint16_t)(1000 + d * 32 + k);
    }
}
static void put(int d, int place, int ordinal) { st.t.t[d][place - 1].glob = (uint16_t)(BASE + ordinal); }
static int fails;
static void expect(const char *what, int d, int place, int want)
{
    int got = st.want[d][place - 1];
    if (got != want) { printf("FAIL %s: division %d place %d moves %d, not %d\n", what, d, place, got, want); fails++; }
}
static void line(const char *name, unsigned it, int nd, const int *sz)
{
    if (name) printf("%s %u", name, it); else printf("%u", it);
    for (int d = 0; d < nd; d++)
        for (int k = 0; k < sz[d]; k++) printf("%c", st.want[d][k] < 0 ? 'u' : st.want[d][k] > 0 ? 'd' : '.');
    printf("\n");
}
static int random_cases(unsigned N)
{
    unsigned changed = 0;
    for (unsigned it = 0; it < N; it++) {
        unsigned short glob[14][32]; int move[14][32];
        make_case(it, glob, move);
        st.t.nd = 14;
        for (int d = 0; d < 14; d++) {
            st.t.n[d] = (uint8_t)SZ[d];
            for (int k = 0; k < SZ[d]; k++) { st.t.t[d][k].glob = glob[d][k]; st.want[d][k] = (int8_t)move[d][k]; }
        }
        sr_b_teams(&st, &LP, &LV, BASE);
        for (int d = 0, ch = 0; d < 14 && !ch; d++)
            for (int k = 0; k < SZ[d]; k++) if (st.want[d][k] != move[d][k]) { changed++; ch = 1; break; }
        line(0, it, 14, SZ);
    }
    for (int c = 0; c < 4; c++) {          /* the countries of SHAPES, no groups */
        const SecShape *sh = &SHAPES[c];
        uint8_t prom[3], rel[3], sec[4][2];
        for (int d = 0; d < sh->nd; d++) { prom[d] = (uint8_t)sh->prom[d]; rel[d] = (uint8_t)sh->rel[d]; }
        for (int i = 0; i < sh->nb; i++) { sec[i][0] = (uint8_t)sh->bt[i][0]; sec[i][1] = (uint8_t)sh->bt[i][1]; }
        const sr_league lp = { (uint8_t)sh->nd, prom, rel, 0, 0, 0, 0, 0, 0, 0,
                               (uint8_t)sh->nb, (const uint8_t (*)[2])sec, (uint8_t)sh->top, (uint8_t)sh->flags };
        for (unsigned it = 0; it < N; it++) {
            unsigned short glob[14][32]; int move[14][32];
            make_shape_case(sh, it, glob, move);
            st.t.nd = (uint8_t)sh->nd;
            for (int d = 0; d < sh->nd; d++) {
                st.t.n[d] = (uint8_t)sh->sz[d];
                for (int k = 0; k < sh->sz[d]; k++) { st.t.t[d][k].glob = glob[d][k]; st.want[d][k] = (int8_t)move[d][k]; }
            }
            sr_b_teams(&st, &lp, 0, (uint16_t)sh->base);
            for (int d = 0, ch = 0; d < sh->nd && !ch; d++)
                for (int k = 0; k < sh->sz[d]; k++) if (st.want[d][k] != move[d][k]) { changed++; ch = 1; break; }
            line(sh->name, it, sh->nd, sh->sz);
        }
    }
    fprintf(stderr, "%u of %u cases changed by the rules\n", changed, 5 * N);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && argv[1][0] == 'r')
        return random_cases((unsigned)atoi(argv[2]));
    /* 1: Juventus NG wins Serie C group A, Juventus in Serie A: up to Serie B */
    tables(); put(0, 10, JUVE); put(2, 1, JUVENG); sr_final_moves(&st, &LP, &LV, BASE);
    expect("B team up, first team above", 2, 1, -1);
    expect("B team up: the 2nd stays", 2, 2, 0);
    /* 2: Juventus in Serie B: its B team may not join it -- held, the 2nd goes up */
    tables(); put(1, 10, JUVE); put(2, 1, JUVENG); sr_final_moves(&st, &LP, &LV, BASE);
    expect("B team held (first team in Serie B)", 2, 1, 0);
    expect("the next club up instead", 2, 2, -1);
    /* 3: Inter relegated from Serie B to Serie C, Inter U23 mid-table in group C: one down,
     *    the best placed relegated of its group stays */
    tables(); put(1, 19, INTER); put(4, 10, INTERU23); sr_final_moves(&st, &LP, &LV, BASE);
    expect("first team down into its level: B team down", 4, 10, 1);
    expect("the best placed relegated stays", 4, 20, 0);
    /* 4: a B team never above Serie B: Juventus NG 1st of Serie B, held */
    tables(); put(0, 5, JUVE); put(1, 1, JUVENG); sr_final_moves(&st, &LP, &LV, BASE);
    expect("B team held (top: Serie B)", 1, 1, 0);
    expect("the 3rd up instead", 1, 3, -1);
    /* 5: the play-offs' moves on top of the record's: Serie B's 3rd up by its play-off */
    tables(); st.mv[0].glob = 1000 + 32 + 2; st.mv[0].delta = -1; st.nmv = 1; sr_final_moves(&st, &LP, &LV, BASE);
    expect("a play-off's winner up", 1, 3, -1);
    printf("%s: B-team rules, %d case(s) failed\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
