/* The Mod's dp_season (tools/dplay.c, included as it is: -DDPLAY_C=<path>) over random final
 * tables (tables.h): Serie B = division 1, Serie C's groups 2..4. Each season's moves. */
#include <stdio.h>
#include <stdlib.h>
#include "tables.h"
#include DPLAY_C
int main(int argc, char **argv)
{
    unsigned N = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    struct trow t[16][32]; int n[16];
    for (unsigned it = 0; it < N; it++) {
        make_tables(it, t, n);
        DP_IN.ng = 3;
        DP_IN.cupw = cup_winner(it, t, n);
        for (int d = 0; d < 4; d++) {
            DP_IN.n[d] = (u8)n[1 + d];
            for (int k = 0; k < n[1 + d]; k++) {
                struct dp_row *r = &DP_IN.t[d][k];
                r->team = t[1 + d][k].team; r->glob = t[1 + d][k].glob; r->won = t[1 + d][k].won;
                r->gf = t[1 + d][k].gf; r->ga = t[1 + d][k].ga; r->pts = t[1 + d][k].pts;
            }
        }
        DP_IN.n[4] = 0;
        dp_season();
        printf("%u", it);
        for (int i = 0; i < DP_OUT.nmv; i++) printf(" d%u:%d", DP_OUT.mv[i].glob, DP_OUT.mv[i].delta);
        printf("\n");
    }
    return 0;
}
