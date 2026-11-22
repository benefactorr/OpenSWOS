/* sr_spec_check (rules/sr_check.c) on broken specs: every real spec of tests/rules/specs accepted;
 * each cut short anywhere refused; targeted faults refused with their code; then random faults --
 * whatever the check lets through is played (the season end, the contests, each club's play-off)
 * over random tables, which `make test` runs under AddressSanitizer: nothing it accepts may read or
 * write past a buffer. Arguments: the spec files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tables.h"
#include "swosrules.h"

static const uint8_t PROM[16] = { 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t REL[16] = { 3, 3, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
static sr_state st;
static unsigned fz = 12345;
static unsigned frand(unsigned n) { fz = fz * 1103515245u + 12345u; return (fz >> 8) % n; }

static uint32_t spec_len(const uint8_t *s, uint32_t cap)    /* a sound spec's length (its 0 included) */
{
    sr_check_error e;
    for (uint32_t n = 1; n <= cap; n++)
        if (sr_spec_check(s, n, 0, &e) == SR_CHECK_OK) return n;
    return 0;
}

/* play a spec the check accepted: the season end over random tables, every play-off's contest and
   every club's play-off */
static void play(const uint8_t *s, uint32_t len, unsigned it)
{
    struct trow t[16][32]; int n[16];
    sr_league lg = { (uint8_t)ND, PROM, REL, 0, 0, 0, 0, s, 0, 0, 0, 0, 0, 0, 0, 0 };
    sr_season se;
    memset(&se, 0, sizeof se);
    lg.spec_len = len;
    make_tables(it, t, n);
    st.t.nd = (uint8_t)ND;
    for (int d = 0; d < ND; d++) {
        st.t.n[d] = (uint8_t)n[d];
        for (int k = 0; k < n[d]; k++) {
            sr_row *r = &st.t.t[d][k];
            r->team = t[d][k].team; r->glob = t[d][k].glob; r->won = t[d][k].won;
            r->gf = t[d][k].gf; r->ga = t[d][k].ga; r->pts = t[d][k].pts;
        }
    }
    se.mgr = t[0][0].team;
    sr_knockouts(&st, &lg, &se);
    {
        static sr_row tab[16][32];
        for (int d = 0; d < ND; d++)
            for (int k = 0; k < n[d]; k++) tab[d][k] = st.t.t[d][k];
        for (int pi = 0; sr_spec_at(&lg, pi); pi++) {
            const sr_row *tabs[3] = { tab[0], tab[ND > 1], tab[ND > 2 ? 2 : 0] };
            int32_t divs[3] = { 0, ND > 1, ND > 2 ? 2 : 0 }, ns[3] = { n[0], n[ND > 1], n[ND > 2 ? 2 : 0] };
            uint16_t team[64], glob[64], pts[64];
            uint8_t prio[64];
            const uint8_t *sp;
            int32_t ne = sr_contest_entrants(&st, &lg, &se, ND > 2 ? 3 : ND, divs, tabs, ns, pi, team, glob, prio, pts, &sp);
            if (ne > 1) {
                sr_contest_seed(&st, pi, team, ne);
                sr_contest_tie(&st, 0, 1, 1, 0, 0);
            }
            sr_group_shape_of(sr_spec_at(&lg, pi), 0);
        }
        for (int d = 0; d < ND; d++)
            for (int k = 0; k < n[d]; k += 3) {
                uint8_t other[9];
                int32_t no;
                sr_club_playoff(&st, &lg, &se, d, tab[d], n[d], tab[d][k].team, other, &no);
            }
    }
}

int main(int argc, char **argv)
{
    int bad = 0, specs = 0, cuts = 0, faults = 0, accepted = 0;
    sr_check_error e;
    sr_reset(&st);
    for (int a = 1; a < argc; a++) {
        static uint8_t s[4096], m[4096];
        FILE *f = fopen(argv[a], "rb");
        size_t got = f ? fread(s, 1, sizeof s, f) : 0;
        if (f) fclose(f);
        uint32_t len = spec_len(s, (uint32_t)got);
        if (!len) { printf("FAIL: %s refused\n", argv[a]); bad++; continue; }
        specs++;
        for (uint32_t cut = 0; cut < len; cut++, cuts++)    /* cut short anywhere: refused */
            if (sr_spec_check(s, cut, 0, &e) == SR_CHECK_OK) { printf("FAIL: %s cut at %u accepted\n", argv[a], cut); bad++; }
        /* targeted faults in play-off 0 (a kind other than the promedio): each its code */
        if (s[0] != 7) {
            const struct { uint32_t at; uint8_t v; int code; } T[] = {
                { 0, 9, SR_CHECK_VALUE },  { 1, 65, SR_CHECK_COUNT }, { 2, 0, SR_CHECK_COUNT },
                { 6, 9, SR_CHECK_VALUE },
            };
            for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
                memcpy(m, s, len);
                m[T[i].at] = T[i].v;
                int r = sr_spec_check(m, len, 0, &e);
                if (r != T[i].code) { printf("FAIL: %s byte %u = %u: code %d, not %d\n", argv[a], T[i].at, T[i].v, r, T[i].code); bad++; }
                faults++;
            }
            {   /* the first round: 65 ties / 33 group members; legs 4; host 4; level 6; a member past the entrants */
                const uint32_t r0 = 6 + 4u * s[1];
                const int grp = s[r0 + 1] & 0x80;
                const struct { uint32_t at; uint8_t v; int code; } R[] = {
                    { r0, (uint8_t)(grp ? 33 : 65), SR_CHECK_COUNT }, { r0 + 1, (uint8_t)(grp ? 0x89 : 4), SR_CHECK_VALUE },
                    { r0 + 2, 4, SR_CHECK_VALUE }, { r0 + 3, 6, SR_CHECK_VALUE }, { r0 + 6, (uint8_t)s[1], SR_CHECK_INDEX },
                };
                for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) {
                    memcpy(m, s, len);
                    m[R[i].at] = R[i].v;
                    int r = sr_spec_check(m, len, 0, &e);
                    if (r == SR_CHECK_OK || (r != R[i].code && r != SR_CHECK_TRUNCATED)) {
                        printf("FAIL: %s round byte %u = %u: code %d, not %d\n", argv[a], R[i].at, R[i].v, r, R[i].code); bad++;
                    }
                    faults++;
                }
            }
        }
        /* random faults: what the check lets through is played (ASan: within every buffer) */
        for (int k = 0; k < 300; k++) {
            memcpy(m, s, len);
            for (int j = 1 + (int)frand(3); j > 0; j--) m[frand(len)] = (uint8_t)frand(256);
            if (sr_spec_check(m, len, 0, &e) == SR_CHECK_OK) { accepted++; play(m, len, (unsigned)k); }
        }
    }
    {   /* a damaged save: a kept table of 33 clubs, a promedio of 49 -- refused, and the season end not played */
        static sr_stage_bank bank;
        static sr_promedio pr;
        static const uint8_t end0[1] = { 0 };
        sr_league lg = { 1, PROM, REL, 0, 0, 0, 0, end0, 0, 0, 0, 0, 0, 0, 0, 1 };
        sr_season se;
        memset(&se, 0, sizeof se);
        se.bank = &bank;
        bank.n_kept = 33;
        if (sr_season_check(&se, &e) != SR_CHECK_COUNT || sr_knockouts(&st, &lg, &se) != -1) {
            printf("FAIL: a kept table of 33 clubs not refused\n"); bad++;
        }
        bank.n_kept = 0;
        se.promedio = &pr;
        pr.n = SR_PROMEDIO_CLUBS + 1;
        if (sr_season_check(&se, &e) != SR_CHECK_COUNT) { printf("FAIL: a promedio of 49 not refused\n"); bad++; }
        faults += 2;
        /* a spec with no length: refused, nothing played; an Italian league out of its divisions:
           sr_italy refuses it (-1) */
        bank.n_kept = 0; se.promedio = 0;
        lg.spec_len = 0;
        if (sr_league_check(&lg, &e) != SR_CHECK_COUNT || sr_knockouts(&st, &lg, &se) != -1) {
            printf("FAIL: a spec with no length not refused\n"); bad++;
        }
        {
            sr_league it = { 3, PROM, REL, 1, 1, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            if (sr_italy(&st, &it, &se) != -1) { printf("FAIL: sr_italy did not refuse Serie C's groups past the league\n"); bad++; }
        }
        faults += 2;
    }
    printf("%s: %d specs accepted, %d cuts and %d faults refused, %d random faults accepted and played%s\n",
           bad ? "FAIL" : "PASS", specs, cuts, faults, accepted, bad ? "" : " within every buffer");
    return bad != 0;
}
