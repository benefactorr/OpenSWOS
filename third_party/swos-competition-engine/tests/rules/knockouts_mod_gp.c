/* The Mod's gp_season (tools/gplay.c, included as it is) over one season's tables: its moves, results,
 * stage titles and SEASON WINNERS page. */
#include <stdio.h>
#include <stdlib.h>
#include GPLAY_C
#ifndef KO_BT
const u8 SEC_TAB[1] = { 0 };            /* (no second teams: play-outs take them anyway) */
#else
/* tables.h's two B teams (globals 444, 445, team words 1420h, 1421h) and their first teams (424,
   425): -DKO_BT=1 a level per division, 2 Italy 2026-27's levels; top KO_BT_TOP, flags KO_BT_FLAGS
   for both. The entries are the Mod 4.0.5's 8 bytes (+6: the team word). */
#if KO_BT == 2
#define BT_LEVELS 14, 0, 1, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3
#else
#define BT_LEVELS 16, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
#endif
const u8 SEC_TAB[] = { 1, BT_LEVELS, 2,
                       0xBC, 0x01, 0xA8, 0x01, KO_BT_TOP, KO_BT_FLAGS, 0x20, 0x14,
                       0xBD, 0x01, 0xA9, 0x01, KO_BT_TOP, KO_BT_FLAGS, 0x21, 0x14 };
static const u8 BT_LV[] = { BT_LEVELS };
#endif
s8 SEC_SLT[4096], SEC_COPY[4096];
const u8 GP_PROM_TAB[1] = { 0 };
u16 GP_TOPW[9];
u8 GP_TOPR[9 * 684];
const u8 GP_HLT[4] = { 0 };
void mod_stages(const unsigned short (*r)[32][6], const int *n, int nk, const unsigned short (*keep)[6], int fresh)
{
    for (int k = 0; k < 4; k++) {
        GP_SSB[0].n[k] = (u8)n[k];
        for (int i = 0; i < n[k]; i++) {
            struct gp_row *x = &GP_SSB[0].r[k][i];
            x->team = r[k][i][0]; x->glob = r[k][i][1]; x->won = r[k][i][2];
            x->gf = r[k][i][3]; x->ga = r[k][i][4]; x->pts = r[k][i][5];
        }
    }
    GP_SSB[0].fresh = (u8)fresh;
    GP_IN.nst[0] = (u8)nk;
    GP_IN.nst[1] = 0;
    for (int i = 0; i < nk; i++) {
        struct gp_row *x = &GP_IN.st[0][i];
        x->team = keep[i][0]; x->glob = keep[i][1]; x->won = 0; x->gf = 0; x->ga = 0; x->pts = keep[i][5];
    }
}
void mod_mgr(unsigned short team) { GP_IN.mgr = team; }   /* (the career club at the season end) */
void mod_gp(const unsigned short (*rows)[32][6], const int *n, int nd, const u8 *sp, FILE *out);
/* The season-end preview (GP_PRE, 4.0.5 rc19) over the same tables first: own = the career club's
   play-off (0xFF none); the SEASON WINNERS entries it appends (after an empty page here). */
void mod_pre(const unsigned short (*rows)[32][6], const int *n, int nd, const u8 *sp, int own, FILE *out)
{
    GP_PRE = 1;
    GP_IN.pi[0] = (u8)own; GP_IN.pi[1] = 0xFF;
    WN_ST.magic = WN_MAGIC; WN_ST.n = 0;
    GP_IN.nd = (u8)nd;
    for (int d = 0; d < nd; d++) {
        GP_IN.n[d] = (u8)n[d];
        for (int k = 0; k < n[d]; k++) {
            struct gp_row *r = &GP_IN.t[d][k];
            r->team = rows[d][k][0]; r->glob = rows[d][k][1]; r->won = rows[d][k][2];
            r->gf = rows[d][k][3]; r->ga = rows[d][k][4]; r->pts = rows[d][k][5];
        }
    }
    GP_IN.spec = sp;
    gp_season();
    fprintf(out, " P");
    for (int i = 0; i < GP_OUT.nmv; i++) fprintf(out, " g%u:%d", GP_OUT.mv[i].glob, GP_OUT.mv[i].delta);
    for (int i = 0; i < GP_OUT.nres; i++)
        fprintf(out, " T%u:%u/%u", GP_OUT.res[i].kind, GP_OUT.res[i].glob[0], GP_OUT.res[i].glob[1]);
    for (int k = 0; k < 3; k++)
        if (GP_OUT.sres[k].kind) fprintf(out, " S%d:%u/%u", k, GP_OUT.sres[k].glob[0], GP_OUT.sres[k].glob[1]);
    fprintf(out, " w");
    for (int i = 0; i < WN_ST.n; i++) fprintf(out, "%02X.%u.%04X,", WN_ST.e[i].type, WN_ST.e[i].idx, WN_ST.e[i].team);
    WN_ST.magic = 0; WN_ST.n = 0;
    GP_PRE = 0;
    GP_IN.pi[0] = GP_IN.pi[1] = 0xFF;
    fprintf(out, " |");
}
void mod_gp(const unsigned short (*rows)[32][6], const int *n, int nd, const u8 *sp, FILE *out)
{
    GP_IN.nd = (u8)nd;
    for (int d = 0; d < nd; d++) {
        GP_IN.n[d] = (u8)n[d];
        for (int k = 0; k < n[d]; k++) {
            struct gp_row *r = &GP_IN.t[d][k];
            r->team = rows[d][k][0]; r->glob = rows[d][k][1]; r->won = rows[d][k][2];
            r->gf = rows[d][k][3]; r->ga = rows[d][k][4]; r->pts = rows[d][k][5];
        }
    }
    GP_IN.spec = sp;
#ifdef KO_BT
    {   /* this season's moves as the game's copy has them at the season end: the record's (the places'
           promotions and relegations, knockouts_ours.c's PROM / REL), in division terms -- up / down one
           level (a level of groups: its first division), out of the lowest: past it */
        static const int PROM[16] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        static const int REL[16] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
        const int nl = BT_LV[0];
        for (int d = 0; d < nd; d++)
            for (int k = 0; k < n[d]; k++) {
                int g = rows[d][k][1], mv = k < PROM[d] && d > 0 ? -1 : k >= n[d] - REL[d] ? 1 : 0, to = d;
                int l = d < nl ? BT_LV[1 + d] : BT_LV[nl] + 1;
                if (mv) {
                    int want = l + mv, j = 0;
                    while (j < nl && BT_LV[1 + j] != want) j++;   /* (the target level's first division) */
                    to = j;
                }
                SEC_SLT[g] = 0;
                SEC_COPY[g] = (s8)(to - d);
            }
    }
#endif
    gp_season();
    for (int i = 0; i < GP_OUT.nmv; i++) fprintf(out, " g%u:%d", GP_OUT.mv[i].glob, GP_OUT.mv[i].delta);
    for (int i = 0; i < GP_OUT.nres; i++)
        fprintf(out, " T%u:%u/%u", GP_OUT.res[i].kind, GP_OUT.res[i].glob[0], GP_OUT.res[i].glob[1]);
    for (int k = 0; k < 3; k++)
        if (GP_OUT.sres[k].kind) fprintf(out, " S%d:%u/%u", k, GP_OUT.sres[k].glob[0], GP_OUT.sres[k].glob[1]);
    if (GP_IN.mgr && WN_ST.magic == WN_MAGIC) {   /* (the SEASON WINNERS page's last season: WN_ST) */
        fprintf(out, " W");
        for (int i = 0; i < WN_ST.n; i++) fprintf(out, "%02X.%u.%04X,", WN_ST.e[i].type, WN_ST.e[i].idx, WN_ST.e[i].team);
        WN_ST.magic = 0;
    }
}
/* The career club's play-off at its league's end (gp_need, a career), for every club of every
   division: its index and the divisions simulated early. */
/* (with B teams: the tables the game has at that moment -- the club's division only, as the library
   is given them: a first team elsewhere is not found) */
static u8 keep_n[GD];
static void only_divs(const int *dv, int nd)
{
#ifdef KO_BT
    for (int d = 0; d < GP_IN.nd; d++) {
        int in = 0;
        for (int j = 0; j < nd; j++) in |= dv[j] == d;
        GP_IN.n[d] = in ? keep_n[d] : 0;
    }
#else
    (void)dv; (void)nd;
#endif
}
static void all_divs(void) { for (int d = 0; d < GP_IN.nd; d++) GP_IN.n[d] = keep_n[d]; }
void mod_need(FILE *out)
{
    GP_IN.gtype = 4;
    for (int d = 0; d < GP_IN.nd; d++) keep_n[d] = GP_IN.n[d];
    for (int d = 0; d < GP_IN.nd; d++)
        for (int k = 0; k < keep_n[d]; k++) {
            GP_IN.mdiv = (u8)d;
            GP_IN.mgr = GP_IN.t[d][k].team;
            only_divs(&d, 1);
            const int need = gp_need();
            all_divs();
            if (!need) continue;
            fprintf(out, " N%d.%d:%d", d, k, GP_IN.pi[0]);
            if (!t2_shape(po_at(GP_IN.pi[0]), 0))
                for (int j = 0; j < SD && GP_IN.sim[j] != NOC; j++) fprintf(out, "/%d", GP_IN.sim[j]);
        }
}

/* The career club's contest (ko_build) for each play-off of knockouts, the club the first entrant
   that waits in round 1: its round-1 ties simulated at the build (presim) -- their winners -- and,
   by record ('host_by_record'), its clubs in rank order. */
static u8 KO_NM[12 * 64];
void mod_ko(FILE *out)
{
    const u8 *s;
    GP_NM = (const u32 *)KO_NM;
    GP_RN = 0;
    GP_TH = 0;
    GP_IN.gtype = 4;
    for (int pi = 0; (s = po_at(pi)); pi++) {
        int n = s[1], grp = 0, wait = -1, in0[64] = { 0 }, ok = 1;
        const u8 *r = s + 6 + 4 * n, *r0 = r;
        u16 team[64];
        for (int k = 0; k < s[2]; k++) { grp |= r[1] & 0x80; r += 6 + 2 * r[0]; }
        {                                       /* (entrants of more than three divisions: not ours) */
            int dv[64], nd = 0;
            for (int i = 0; i < n; i++) {
                int j = 0;
                while (j < nd && dv[j] != s[7 + 4 * i]) j++;
                if (j == nd) dv[nd++] = s[7 + 4 * i];
            }
            grp |= nd > 3;
        }
        if (grp || r0[4] || n > 64) continue;   /* (group rounds, a paired round 1: not here) */
        int dv[64], ndv = 0;
        for (int i = 0; i < n; i++) {
            int j = 0;
            while (j < ndv && dv[j] != s[7 + 4 * i]) j++;
            if (j == ndv) dv[ndv++] = s[7 + 4 * i];
        }
        for (int d = 0; d < GP_IN.nd; d++) keep_n[d] = GP_IN.n[d];
        only_divs(dv, ndv);
        for (int i = 0; i < n; i++) {
            int c = ent_club(s + 6 + 4 * i, s[0]);
            if (c < 0) ok = 0; else team[i] = row(c)->team;
        }
        if (!ok) { all_divs(); continue; }
        for (int i = 0; i < r0[0]; i++)
            for (int x = 0; x < 2; x++) if (r0[6 + 2 * i + x] < n) in0[r0[6 + 2 * i + x]] = 1;
        for (int i = 0; i < n && wait < 0; i++) {
            if (in0[i]) continue;
            wait = i;
            for (int j = 0; j < n; j++) if (j != i && team[j] == team[i]) wait = -1;
        }
        if (wait < 0) { all_divs(); continue; }
        GP_IN.mgr = team[wait];
        struct gp_ko *k = &GP_ST.ko[0];
        ko_build(k, pi, 4, 0xFE);
        all_divs();
        fprintf(out, " K%d:", pi);
        for (int t = 0; t < r0[0]; t++) fprintf(out, "%04X,", k->tw[t] != NOC ? k->team[k->tw[t]] : 0);
        if (s[5] & 2)
            for (int p = 0; p < 256; p++)
                for (int j = 0; j < k->n; j++) if (k->prio[j] == p) fprintf(out, "r%04X", k->team[j]);
    }
}

/* the contests' checks leave the season's state as they found it (gp_need picks a bank, ko_build
   marks GP_ST): the next season's gp_season must not see them */
static struct gp_state ko_st;
static struct gp_in ko_in;
static unsigned char ko_ssb[sizeof GP_SSB];
void mod_ko_all(FILE *out)
{
    struct gp_ss *ssb = SSB;
    ko_st = GP_ST; ko_in = GP_IN;
    for (unsigned i = 0; i < sizeof GP_SSB; i++) ko_ssb[i] = ((unsigned char *)GP_SSB)[i];
    mod_ko(out);
    mod_need(out);
    GP_ST = ko_st; GP_IN = ko_in; SSB = ssb;
    for (unsigned i = 0; i < sizeof GP_SSB; i++) ((unsigned char *)GP_SSB)[i] = ko_ssb[i];
}
