/* The library's parallel groups (sr_pg_season) over groups_cases.h's season ends: the Mod's table
 * as an sr_pg_league, a club by its index in it, an entering club's areas its new identity's.
 * Arguments: TABLE PROMREL SEASONS. */
#include "swosrules.h"
#include "groups_cases.h"

static sr_pg_level s_level[GC_PL];
static sr_pg_club s_club[GC_CLUBS];

int main(int argc, char **argv)
{
    static int8_t old_off[GC_CLUBS], new_off[GC_CLUBS];
    static uint8_t entering[GC_CLUBS];
    sr_pg_league pg;
    if (argc < 4) return 2;
    gc_load(argv[1], argv[2]);
    for (int l = 0; l < gc_nl; l++) {
        s_level[l].first = (uint8_t)gc_first[l];
        s_level[l].groups = (uint8_t)gc_cnt[l];
        for (int k = 0; k < gc_cnt[l] && k < SR_PG_GROUPS; k++) s_level[l].size[k] = (uint8_t)gc_size[gc_first[l] + k];
    }
    pg.levels = (uint8_t)gc_nl; pg.level = s_level; pg.clubs = (uint16_t)gc_nc; pg.club = s_club;
    for (unsigned it = 0, n = (unsigned)atoi(argv[3]); it < n; it++) {
        gc_season(it);
        for (int i = 0; i < gc_nc; i++) {
            s_club[i].base = (uint8_t)gc_base[i];
            for (int p = 0; p < SR_PG_PARALLEL; p++)    /* (this season's: an entering club's new identity's) */
                s_club[i].area[p] = p < gc_npar ? gc_run[gc_area_at[i] + p] : 0xFF;
            old_off[i] = gc_slt[gc_g[i]];
            new_off[i] = gc_copy[gc_g[i]];
            entering[i] = 0;
        }
        for (int i = 0; i < gc_nenter; i++) entering[gc_enter[i]] = 1;
        if (sr_pg_season(&pg, old_off, new_off, entering) != 0) { printf("refused\n"); return 1; }
        for (int i = 0; i < gc_nc; i++) gc_copy[gc_g[i]] = new_off[i];
        gc_print(it);
    }
    return 0;
}
