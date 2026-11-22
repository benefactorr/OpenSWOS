#ifndef SWOS2027_MODERN_CAREER_ROSTER_H
#define SWOS2027_MODERN_CAREER_ROSTER_H
#include "modern_world.h"
/* A match is a temporary 16-slot view into a persistent 25+ player club.
 * The map is kept by player UUID, not squad ordinal: reordering the full
 * roster cannot silently assign a different player's match changes. */
typedef struct ModernMatchSelection {
    char club_id[MODERN_ID_LEN];
    char player_ids[MODERN_MATCH_PLAYERS][MODERN_ID_LEN];
} ModernMatchSelection;

/* Pick a SWOS-compatible 16 with goalkeeper preference and position cover.
 * Does not change any team, player or source record. */
/* Automatic 4-4-2 selection, independent of a previous manager's saved XI. */
int ModernCareer_SelectAI16(const ModernRoster *roster, size_t slots[MODERN_MATCH_PLAYERS]);
int ModernCareer_Select16(const ModernRoster *roster, size_t slots[MODERN_MATCH_PLAYERS]);
/* Validate/set a custom match selection, preserving all club players. */
int ModernCareer_SetPreferred16(ModernRoster *roster, const size_t slots[MODERN_MATCH_PLAYERS]);
int ModernCareer_Preferred16(const ModernRoster *roster, size_t slots[MODERN_MATCH_PLAYERS]);
void ModernCareer_ClearPreferred16(ModernRoster *roster);
/* Project and remember 16 stable identities for committing edited match state. */
int ModernCareer_BeginMatch(const ModernRoster *roster,
                             const size_t slots[MODERN_MATCH_PLAYERS],
                             TeamFileEx *match, ModernMatchSelection *selection);
/* Write back exactly the sixteen selected player records to their IDs.
 * Other players, ages, reserves and club identity remain unchanged. */
int ModernCareer_CommitMatch(ModernRoster *roster, const TeamFileEx *match,
                             const ModernMatchSelection *selection);
/* Explicit cross-club movement; never silently discard or replace players. */
int ModernCareer_Transfer(ModernWorld *world, const char *player_id,
                          const char *from_club, const char *to_club);
int ModernCareer_SellWithLoanCover(ModernWorld *world, const char *player_id,
                                  const char *from_club, const char *to_club);
ModernRosterPlayer ModernCareer_NewLoan(const ModernWorld *world, const ModernRoster *club,
                                       const PlayerFile *origin, unsigned position, int shirt);
void ModernCareer_ReplacementSkills(const ModernRoster *club, PlayerFile *player,
                                   const PlayerFile *retired, uint32_t seed);
void ModernCareer_NameReplacement(const ModernWorld *world, PlayerFile *player,
                                  const PlayerFile *origin, uint32_t seed);
/* Check every full roster for duplicate IDs, minimum of 16 players and a GK. */
int ModernCareer_ValidateWorld(const ModernWorld *world);
/* Copy snapshot to a NEW path only (refuses to overwrite a previous save). */
int ModernCareer_SaveSnapshot(const ModernWorld *world, const char *path);
#endif
