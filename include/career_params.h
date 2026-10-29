#ifndef CAREER_PARAMS_H
#define CAREER_PARAMS_H

/* swos_types.h, NOT <stdint.h>: the Amiga toolchain has no libc <stdint.h> (it provides the
 * int*_t types itself). Including <stdint.h> here breaks the Amiga build. */
#include "swos_types.h"

/* Hard ceiling on career length = the compile-time size of seasonInformationsArray.
 * num_seasons (below) must be <= this. */
#define CAREER_MAX_SEASONS 35

/* Tunable career-mode balance knobs.
 *
 * The defaults (career_params.c) reproduce the original hard-coded SWOS values, so the
 * game behaves identically until bin/data_json/career_params.json overrides a field at
 * startup (DataJson_LoadCareerParams, SDL build). Testers edit that JSON and relaunch —
 * no rebuild. See docs/DATA_PIPELINE.md. Compiled on both ports; only the JSON *loader*
 * is SDL-only (Amiga always uses the defaults). */
typedef struct {
    /* ---- Finances: starting balance (CalculateStartingBalance) ----
     * balance = trunc16(teamValueBase * value_mult)
     *         * (pct_min + (rand8 * pct_range >> 8)) / 100 * scale */
    uint16_t start_balance_value_mult;   /* 3 */
    uint16_t start_balance_pct_min;      /* 80 */
    uint16_t start_balance_pct_range;    /* 40  -> 80..119% */
    uint16_t start_balance_scale;        /* 1000 */

    /* ---- Finances: match gate receipts (calc{League,Cup}MatchGateReceiptsFactor) ----
     * factor = (homeVal/divisor * mult_home + awayVal/divisor * mult_away) >> 4 / teamValueDivisor */
    uint16_t gate_value_divisor;         /* 4000 */
    uint16_t gate_league_mult_home;      /* 10000 */
    uint16_t gate_league_mult_away;      /* 2000 */
    uint16_t gate_cup_mult;              /* 6000 (both teams) */

    /* ---- Finances: player wages (Career_UpdatePlayerWeeklyWageCache) ----
     * wage = avgValue (×trialist%/reserve% if applicable) * 1000 / 768 * scale / wageScalingDivisor */
    uint16_t wage_trialist_pct;          /* 33 */
    uint16_t wage_reserve_pct;           /* 66 */
    uint16_t wage_scale;                 /* 96 */

    /* ---- Board / chairman expectations (ShowChairmanOpinionAboutTheSeason) ----
     * verdict category from ratio = (rank - leaguePosition) * 100 / numTeams (lower position = better) */
    int16_t  board_ratio_ok;             /* 30   ratio <  ok       -> at least cat 1 */
    int16_t  board_ratio_good;           /* 10   ratio <  good     -> at least cat 2 */
    int16_t  board_ratio_poor;           /* -10  ratio <= poor     -> at least cat 3 */
    int16_t  board_ratio_terrible;       /* -30  ratio <= terrible -> cat 4 */
    uint16_t board_percentile_low;       /* 25   position percentile <= -> verdict improves one step */
    uint16_t board_percentile_high;      /* 75   position percentile >= -> verdict worsens one step */
    uint16_t board_secondary_top2_bonus; /* 2    secondary-cup top-2 finish improves verdict */
    uint16_t board_tertiary_top2_bonus;  /* 2    tertiary-cup top-2 */
    uint16_t board_euro_top2_bonus;      /* 3    euro-cup top-2 */

    /* ---- Career length ---- */
    uint16_t num_seasons;                /* 35 (default), capped by CAREER_MAX_SEASONS — the
                                          * career retires after this many seasons */
} CareerParams;

extern CareerParams _careerParams;

#endif /* CAREER_PARAMS_H */
