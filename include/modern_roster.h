#ifndef SWOS2027_MODERN_ROSTER_H
#define SWOS2027_MODERN_ROSTER_H
/* Expanded career roster, NEVER passed as a TeamFileEx to the match engine. */
#include <stddef.h>
#include <stdint.h>
#include "game_types.h"
#define MODERN_SQUAD_TARGET 25
#define MODERN_MATCH_PLAYERS 16
#define MODERN_ID_LEN 37
typedef struct ModernRosterPlayer {
    char id[MODERN_ID_LEN];
    PlayerFile swos;
    uint8_t age;
    uint8_t is_reserve; /* generated trial or reserve player */
    uint32_t last_ai_transfer;
} ModernRosterPlayer;
typedef struct ModernRoster {
    char club_id[MODERN_ID_LEN];
    TeamFile header; /* unchanged 76-byte SWOS team header */
    ModernRosterPlayer *players;
    size_t count, capacity;
    /* R7: user-selected match-day IDs. Persisted in R07 roster saves. */
    uint8_t has_preferred16;
    char preferred16[MODERN_MATCH_PLAYERS][MODERN_ID_LEN];
} ModernRoster;
/* All 25+ squad players are retained in this store, including AI clubs. */
void ModernRoster_Init(ModernRoster *r, const char *club_id, const TeamFile *header);
int ModernRoster_Add(ModernRoster *r, const ModernRosterPlayer *p);
int ModernRoster_Remove(ModernRoster *r, const char *id);
void ModernRoster_Free(ModernRoster *r);
/* The caller chooses the 16 distinct player indexes. No other players change. */
int ModernRoster_MatchTeam(const ModernRoster *r, const size_t slots[16], TeamFileEx *out);
#endif
