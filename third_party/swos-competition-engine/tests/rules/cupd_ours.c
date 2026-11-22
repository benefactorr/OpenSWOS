/* The library's Coppa Italia Serie D entry order (sr_cupd_keep_place, sr_cupd_note_arrival,
 * sr_cupd_make) over the cases of cupd_cases.h: each later season's first two rounds, printed as
 * cupd_mod.c prints the Mod's cd_rank / cd_season / cd_make. */
#include <stdio.h>
#include <stdlib.h>
#include "swosrules.h"
#include "cupd_cases.h"
static sr_cupd_end end;
int main(int argc, char **argv)
{
    unsigned N = argc > 1 ? (unsigned)atoi(argv[1]) : 20000;
    struct cd_case cc;
    uint8_t division[SR_CLUB_SLOTS];
    uint16_t word[SR_CLUB_SLOTS];
    sr_cupd_rounds out;
    for (int i = 0; i < SR_CLUB_SLOTS; i++) word[i] = (uint16_t)(20 | i << 8);   /* (file 20, ordinal i) */
    for (unsigned it = 0; it < N; it++) {
        make_cd_case(it, &cc);
        sr_cupd_end_reset(&end);
        for (int d = 0; d < 14; d++)
            for (int k = 0; k < cc.n[d]; k++)
                sr_cupd_keep_place(&end, CD_FIRST, CD_GROUPS, (uint16_t)d, cc.order[d][k], k, cc.n[d]);
        for (int i = 0; i < 256; i++)
            if (cc.arrival[i >> 3] & (1 << (i & 7))) sr_cupd_note_arrival(&end, (uint16_t)i);
        for (int i = 0; i < SR_CLUB_SLOTS; i++) division[i] = i < 256 ? (uint8_t)(cc.base[i] + cc.next[i]) : 0xFF;
        sr_cupd_make(&end, (uint32_t)cc.season, CD_ROUNDS, CD_FIRST, CD_GROUPS, SR_CLUB_SLOTS, division, word, &out);
        printf("%u %u:", it, out.np);
        for (int t = 0; t < out.np + 64; t++) printf(" %04x.%04x", out.side[2 * t], out.side[2 * t + 1]);
        printf("\n");
    }
    return 0;
}
