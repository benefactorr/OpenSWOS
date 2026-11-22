/* The library's season end (sr_italy, then sr_knockouts) over the same random final tables
 * (tables.h): each season's moves, results, stage titles and SEASON WINNERS page, as
 * knockouts_mod.c prints the Mod's. Arguments: SPEC [SEASONS [SHAPE [ko]]]. */
#include <stdio.h>
#include <stdlib.h>
#include "tables.h"
#include "swosrules.h"
static const uint8_t PROM[16] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t REL[16] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static sr_state st;
static sr_stage_bank bank;
static sr_promedio prom;                /* (the history: kept season to season, no seed) */
static sr_winners wn;
static void copy_row(sr_row *y, const struct trow *x)
{
    y->team = x->team; y->glob = x->glob; y->won = x->won; y->gf = x->gf; y->ga = x->ga; y->pts = x->pts;
}

/* The game's "selected team" as OpenSWOS's check drove it: the career club at the season end,
 * then the contests' clubs; what each contest call takes as the career club. */
static uint16_t sel;

/* The career club's contest for each play-off (sr_contest_entrants and its presim), the club the
 * first entrant that waits in round 1; then each club's play-off (sr_club_playoff): as mod_ko_all. */
static void ours_ko(const sr_league *lg, sr_season *se, struct trow (*t)[32], const int *n)
{
    static sr_row tab[16][32];
    const sr_row *tabs[3];
    int32_t divs[3], ns[3];
    for (int d = 0; d < ND; d++)
        for (int k = 0; k < n[d]; k++) copy_row(&tab[d][k], &t[d][k]);
    const uint8_t *s;
    for (int pi = 0; (s = sr_spec_at(lg, pi)) != 0; pi++) {
        int ne = s[1], grp = 0, wait = -1, in0[64] = { 0 };
        const uint8_t *r = s + 6 + 4 * ne, *r0 = r, *sp = 0;
        uint16_t team[64], glob[64], pts[64];
        uint8_t prio[64];
        for (int k = 0; k < s[2]; k++) { grp |= r[1] & 0x80; r += 6 + 2 * r[0]; }
        int dv[3], nd = 0;
        for (int i = 0; i < ne && !grp; i++) {  /* (its entrants' divisions: at most three) */
            int j = 0;
            while (j < nd && dv[j] != s[7 + 4 * i]) j++;
            if (j == nd) { if (nd < 3) dv[nd++] = s[7 + 4 * i]; else grp = 1; }
        }
        if (grp || r0[4] || ne > 64) continue;
        sel = 0xFFFF;
        se->mgr = sel;
        for (int j = 0; j < nd; j++) { divs[j] = dv[j]; tabs[j] = tab[dv[j]]; ns[j] = n[dv[j]]; }
        if (dv[0] >= ND || sr_contest_entrants(&st, lg, se, nd, divs, tabs, ns, pi, team, glob, prio, pts, &sp) != ne)
            continue;
        for (int i = 0; i < r0[0]; i++)
            for (int x = 0; x < 2; x++) if (r0[6 + 2 * i + x] < ne) in0[r0[6 + 2 * i + x]] = 1;
        for (int i = 0; i < ne && wait < 0; i++) {
            if (in0[i]) continue;
            wait = i;
            for (int j = 0; j < ne; j++) if (j != i && team[j] == team[i]) wait = -1;
        }
        if (wait < 0) continue;
        sel = team[wait];
        se->mgr = sel;
        sr_contest_entrants(&st, lg, se, nd, divs, tabs, ns, pi, team, glob, prio, pts, &sp);
        sr_contest_seed(&st, pi, team, ne);
        printf(" K%d:", pi);
        for (int i = 0; i < r0[0]; i++) {
            const int a = r0[6 + 2 * i], b = r0[7 + 2 * i];
            int w = -1;
            if (a < ne && b < ne)
                w = team[a] == team[b] ? a : sr_contest_tie(&st, a, b, r0[1], r0[2] & 0x7F, r0[3]);
            printf("%04X,", w >= 0 ? team[w] : 0);
        }
        if (s[5] & 2)
            for (int p = 0; p < 256; p++)
                for (int j = 0; j < ne; j++) if (prio[j] == p) printf("r%04X", team[j]);
    }
    se->mgr = sel;
    for (int d = 0; d < ND; d++)                /* (the club's play-off: gp_need, each club) */
        for (int k = 0; k < n[d]; k++) {
            uint8_t other[9];
            int32_t no = 0;
            const int32_t pi = sr_club_playoff(&st, lg, se, d, tab[d], n[d], tab[d][k].team, other, &no);
            if (pi == -1) continue;
            printf(" N%d.%d:%d", d, k, pi < -1 ? -2 - pi : pi);
            for (int j = 0; pi >= 0 && j < no; j++) printf("/%d", other[j]);
        }
}
int main(int argc, char **argv)
{
    unsigned N = argc > 2 ? (unsigned)atoi(argv[2]) : 20000;
    struct trow t[16][32]; int n[16];
    int pre_it = 0;                             /* (the season's number printed with its preview) */
    sr_league lg = { 14, PROM, REL, 1, 1, 2, 3, spec, 0, 0 };
#ifdef KO_BT                                    /* (tables.h's B teams: knockouts_mod_gp.c's SEC_TAB) */
    static const uint8_t SECOND[2][2] = { { 20, 0 }, { 21, 1 } };
    static const uint16_t SECOND_TEAM[2] = { 0x1420, 0x1421 };
    static const uint8_t IT_FIRST[4] = { 0, 1, 2, 5 };
    static const sr_levels IT_LV = { 4, IT_FIRST };
    lg.seconds = 2; lg.second = SECOND; lg.second_top = KO_BT_TOP; lg.second_flags = KO_BT_FLAGS;
    lg.second_team = SECOND_TEAM;
#endif
    load_spec(argv[1]);
    lg.spec_len = spec_size;
    if (argc > 3) { set_shape(argv[3]); lg.divisions = (uint8_t)ND; lg.italy = 0; }
    sr_reset(&st);
    for (unsigned it = 0; it < N; it++) {
        sr_season se = { 0 };
        make_tables(it, t, n);
#ifdef KO_BT
        bt_place(it, t, n);
#endif
        se.cup_winner = cup_winner(it, t, n);
        st.t.nd = (uint8_t)ND;
        for (int d = 0; d < ND; d++) {
            st.t.n[d] = (uint8_t)n[d];
            for (int k = 0; k < n[d]; k++) copy_row(&st.t.t[d][k], &t[d][k]);
        }
        {
            struct stages s;
            make_stages(it, t, n, &s);
            bank.used = 1; bank.fresh = (uint8_t)s.fresh;
            for (int k = 0; k < 4; k++) {
                bank.n[k] = (uint8_t)s.n[k];
                for (int i = 0; i < s.n[k]; i++) copy_row(&bank.r[k][i], &s.r[k][i]);
            }
            bank.kept_div = 0; bank.n_kept = (uint8_t)s.nkeep;
            for (int i = 0; i < s.nkeep; i++) {
                bank.kept[i].team = s.keep[i].team; bank.kept[i].glob = s.keep[i].glob;
                bank.kept[i].won = bank.kept[i].gf = bank.kept[i].ga = 0; bank.kept[i].pts = s.keep[i].pts;
            }
        }
        const uint16_t saved = sel;
        {                                       /* (the career club: a club of each division in turn) */
            const int d = (int)(it % ND);
            sel = n[d] ? t[d][(it / ND) % n[d]].team : 0;
            se.mgr = sel;
        }
        se.bank = &bank;
#ifdef KO_BT
        se.b_in_ko = 1;
        se.base = 424;
        se.levels = KO_BT == 2 ? &IT_LV : 0;
#endif
        se.promedio = &prom;
        se.winners = &wn;
        if (getenv("KO_PRE")) {                 /* (the preview first, as knockouts_mod.c) */
            sr_season pre = se;
            pre.preview = 1;
            pre.own_pi[0] = (uint8_t)(it % 2 ? 0 : 1); pre.own_pi[1] = 0;   /* (play-off 0 + 1, or none) */
            wn.n = 0;
            st.nmv = st.dp_moves = 0;
            sr_knockouts(&st, &lg, &pre);
            printf("%u P", it);
            pre_it = 1;
            for (int i = 0; i < st.nmv; i++) printf(" g%u:%d", st.mv[i].glob, st.mv[i].delta);
            for (int i = 0; i < st.gp.nres; i++)
                printf(" T%u:%u/%u", st.gp.res[i].kind, st.gp.res[i].glob[0], st.gp.res[i].glob[1]);
            for (int k = 0; k < 3; k++)
                if (st.gp.sres[k].kind) printf(" S%d:%u/%u", k, st.gp.sres[k].glob[0], st.gp.sres[k].glob[1]);
            printf(" w");
            for (int i = 0; i < wn.n; i++) printf("%02X.%u.%04X,", wn.e[i].type, wn.e[i].idx, wn.e[i].team);
            printf(" |");
            wn.n = 0; wn.fresh = 0;
            st.nmv = st.dp_moves = 0;
        }
        sr_italy(&st, &lg, &se);
        sr_knockouts(&st, &lg, &se);
        if (!pre_it) printf("%u", it);
        pre_it = 0;
        for (int i = 0; i < st.nmv; i++)
            printf(" %c%u:%d", i < st.dp_moves ? 'd' : 'g', st.mv[i].glob, st.mv[i].delta);
        for (int i = 0; i < st.gp.nres; i++)
            printf(" T%u:%u/%u", st.gp.res[i].kind, st.gp.res[i].glob[0], st.gp.res[i].glob[1]);
        for (int k = 0; k < 3; k++)
            if (st.gp.sres[k].kind) printf(" S%d:%u/%u", k, st.gp.sres[k].glob[0], st.gp.sres[k].glob[1]);
        if (se.mgr && wn.fresh) {               /* (the SEASON WINNERS page's last season) */
            printf(" W");
            for (int i = 0; i < wn.n; i++) printf("%02X.%u.%04X,", wn.e[i].type, wn.e[i].idx, wn.e[i].team);
            wn.fresh = 0;
        }
        sel = saved;
        if (argc > 4) ours_ko(&lg, &se, t, n);
        printf("\n");
    }
    return 0;
}
