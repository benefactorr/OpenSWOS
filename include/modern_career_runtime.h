#ifndef SWOS2027_MODERN_CAREER_RUNTIME_H
#define SWOS2027_MODERN_CAREER_RUNTIME_H
#include "modern_career_roster.h"
/* Opt-in live career bridge. Old TEAM buffers retain their precise SWOS layout.
 * A binding remembers sixteen UUIDs, not sixteen arbitrary roster offsets. */
int ModernRuntime_Requested(void);
int ModernRuntime_Enabled(void);
ModernRoster *ModernRuntime_Roster(ModernWorld *world, uint8_t file, uint8_t ordinal);
int ModernRuntime_ActiveClub(const ModernRoster *roster, uint16_t *team);
unsigned ModernRuntime_PlayerAge(ModernWorld *world, const TeamFileEx *team, unsigned slot);
unsigned ModernAI_RunTransfers(uint32_t seed, int show_summary);
void ModernAI_TransfersMenu(void);
void ModernSearch_Open(void);
void ModernSearch_National(void);
void ModernSearch_Offer(uint16_t team, const char *player_id);
void ModernRuntime_ProjectOpponent(ModernWorld *world, TeamFileEx *team);
void ModernRuntime_RefreshOpponent(ModernWorld *world, TeamFileEx *team, uint16_t managedTeam);
uint32_t ModernRuntime_ScaleOpeningBalance(ModernWorld *world, uint16_t team,
                                          unsigned division, uint32_t balance);
int ModernRuntime_Activate(ModernWorld *world, TeamFileEx *legacy_team);
/* R7: Atomically re-project the chosen 16 into a bound SWOS team. */
int ModernRuntime_Choose16(ModernWorld *world, TeamFileEx *legacy_team,
                           const size_t slots[MODERN_MATCH_PLAYERS]);
int ModernRuntime_ExpandCareerSquad(ModernWorld *world, TeamFileEx *team);
/* Append to the UUID roster and the native squad without renumbering players. */
int ModernRuntime_AppendCareerPlayer(ModernWorld *world, TeamFileEx *team,
                                    const ModernRosterPlayer *player);
/* Rebind after a roster change, retaining the manager's surviving slots. */
int ModernRuntime_RebindCareerSquad(ModernWorld *world, TeamFileEx *team,
                                    const char previous[32][MODERN_ID_LEN],
                                    const uint8_t order[32], size_t previous_count);
int ModernRuntime_CommitCareerSquad(ModernWorld *world, TeamFileEx *team);
int ModernRuntime_Flush(ModernWorld *world, TeamFileEx *legacy_team);
int ModernRuntime_FlushAll(ModernWorld *world);
void ModernRuntime_Reset(void);
size_t ModernRuntime_BindingCount(void);
/* Protect live match UUIDs against transfers from stale 16-player caches. */
int ModernRuntime_PlayerIsBound(const char *player_uuid);
/* Resolve a physical 0..15 slot using the active binding, never by name or
 * player byte comparison. Returns 0 if this club was not projected safely. */
/* Flush and retire a live match projection before transferring one of its
 * players. Caller may reproject with ModernRuntime_Activate afterwards. */
int ModernRuntime_Suspend(ModernWorld *world,TeamFileEx *team);
int ModernRuntime_PlayerUUID(const TeamFileEx *team,unsigned slot,
                             char out[MODERN_ID_LEN]);
#endif
