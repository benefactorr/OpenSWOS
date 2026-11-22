#ifndef SWOS2027_MODERN_WORLD_H
#define SWOS2027_MODERN_WORLD_H
#include "modern_roster.h"
/* The native career world owns ALL players, including AI reserves.
 * A 16-player projection is produced only when a match needs it.
 * Roster world format version 2 (SW27R02) is independent of .CAR saves.
 */
typedef struct ModernWorld {
    ModernRoster *clubs;
    size_t count;
    /* Lazy O(1) lookup for SWOS' 8-bit country and team ordinals. */
    uint32_t *swos_lookup;
    uint32_t ai_transfer_run;
} ModernWorld;
ModernWorld *ModernWorld_Active(void);
int ModernWorld_Load(ModernWorld *world, const char *path);
int ModernWorld_Save(const ModernWorld *world, const char *path);
void ModernWorld_Free(ModernWorld *world);
ModernRoster *ModernWorld_Find(ModernWorld *world, const char *club_uuid);
ModernRoster *ModernWorld_FindSwos(ModernWorld *world, unsigned country, unsigned ordinal);
int ModernWorld_Match(const ModernWorld *world, const char *club_uuid,
                      const size_t selection[16], TeamFileEx *out);
#endif
