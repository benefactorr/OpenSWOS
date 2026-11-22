/* sr_pg_season with 4.0.6's pg_a2 guard: an area byte whose 2nd choice is past G -- or a ban with no
 * 2nd choice, the same byte (8h) -- has no 2nd choice. 4.0.5 read it as group -1 (occ[tl][-1]); no
 * real data has it, so the Mod's C is no reference here: built cases, under the sanitizers
 * (make test). A league: one division above a level of two groups of one club each, both full;
 * the top club comes down with such an area, the clubs in the groups have it too, so no room can
 * be made and no 2nd choice taken -- it goes to the nearest group with room, none: its 1st. */
#include <stdio.h>
#include "swosrules.h"

int main(void)
{
    static const sr_pg_level LV[2] = { { 0, 1, { 1 } }, { 1, 2, { 1, 1 } } };
    int bad = 0;
    for (int form = 0; form < 2; form++) {
        /* form 0: 'A' with a ban and no 2nd (80h); form 1: 1st A, 2nd H (H + 1 = 8: 80h too, by
           another way of writing it -- the same byte, the same meaning) */
        const uint8_t ab = form == 0 ? 0x80 : (uint8_t)((7 + 1) << 4 | 0);
        sr_pg_club club[3] = { { 0, { ab, 0xFF, 0xFF, 0xFF } }, { 1, { ab, 0xFF, 0xFF, 0xFF } },
                               { 2, { 0x81, 0xFF, 0xFF, 0xFF } } };
        const sr_pg_league pg = { 2, LV, 3, club };
        int8_t old_off[3] = { 0, 0, 0 }, new_off[3] = { 1, 0, 0 };   /* (club 0: one division down) */
        if (sr_pg_season(&pg, old_off, new_off, 0) != 0 || new_off[0] + club[0].base != 1 ||
            new_off[1] + club[1].base != 1 || new_off[2] + club[2].base != 2) {
            printf("FAIL: form %d: divisions %d %d %d (want 1 1 2)\n", form, new_off[0] + club[0].base,
                   new_off[1] + club[1].base, new_off[2] + club[2].base);
            bad++;
        }
    }
    {   /* a league past the limits: refused, nothing changed */
        static const sr_pg_level BIG[1] = { { 0, SR_PG_GROUPS + 1, { 0 } } };
        sr_pg_club c = { 0, { 0xFF, 0xFF, 0xFF, 0xFF } };
        const sr_pg_league pg = { 1, BIG, 1, &c };
        int8_t o = 0, n = 0;
        if (sr_pg_season(&pg, &o, &n, 0) != -1) { printf("FAIL: %d groups not refused\n", SR_PG_GROUPS + 1); bad++; }
    }
    printf("%s: sr_pg_season, a 2nd choice past G or a ban with no 2nd read as none (4.0.6's pg_a2 guard),"
           " no group -1; a league past the limits refused\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}
