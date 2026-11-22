#ifndef SWOS2027_MODERN_SEASON_H
#define SWOS2027_MODERN_SEASON_H
#include "modern_world.h"
/* Advance exactly one completed native career season. Ages remain within
 * the roster format's 14..60 range. Retired players receive replacements. */
typedef struct ModernSeasonStats {
    size_t clubs, players, aged, retired;
} ModernSeasonStats;
/* Called with a copy of the outgoing player after ageing, and the new
 * player in the same roster slot. */
typedef void (*ModernRetirementObserver)(const ModernRoster *roster,
                                          const ModernRosterPlayer *retired,
                                          const ModernRosterPlayer *replacement,
                                          void *context);
int ModernSeason_AdvanceWithRetirements(ModernWorld *world, unsigned season,
                                       ModernSeasonStats *stats,
                                       ModernRetirementObserver observe, void *context);
int ModernSeason_Advance(ModernWorld *world, unsigned season,
                         ModernSeasonStats *stats);
#endif
