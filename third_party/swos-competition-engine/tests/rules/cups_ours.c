/* The library's sr_cup_entrants over the cases of cups_cases.h (Italy 2026-27's two cups), each
 * cup's clubs printed as cups_mod.c prints the Mod's cf_build. */
#include <stdio.h>
#include <stdlib.h>
#include "swosrules.h"
#include "cups_cases.h"
int main(int argc, char **argv)
{
    unsigned N = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    struct cup_case cc;
    sr_quota q[2][3];
    uint16_t slot[256];
    uint8_t division[256], level[16] = { 0 };
    for (int c = 0; c < 2; c++)
        for (int g = 0; g < CUP_G[c]; g++) {
            q[c][g].division = CUP_Q[c][g][0]; q[c][g].clubs = CUP_Q[c][g][1]; q[c][g].byes = CUP_Q[c][g][2];
        }
    for (int d = 0; d < 14; d++) level[d] = (uint8_t)LEVEL[d];
    for (unsigned it = 0; it < N; it++) {
        make_case(it, &cc);
        for (int i = 0; i < 256; i++) { slot[i] = (uint16_t)i; division[i] = (uint8_t)(cc.base[i] + cc.delta[i]); }
        printf("%u", it);
        for (int c = 0; c < 2; c++) {
            uint16_t out[SR_CUP_CLUBS];
            int n = sr_cup_entrants(q[c], CUP_G[c], CUP_N[c], 256, slot, division, cc.ranked ? cc.place : 0,
                                    level, cc.career, out);
            printf(" |");
            for (int k = 0; k < n; k++) printf(" %u", out[k]);
        }
        printf("\n");
    }
    return 0;
}
