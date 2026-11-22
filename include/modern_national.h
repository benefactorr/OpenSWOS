#ifndef SWOS_MODERN_NATIONAL_H
#define SWOS_MODERN_NATIONAL_H

#include "modern_world.h"

/* The 2026/27 world keeps the original national competition slots, but
 * some of their nationality and domestic-file codes have changed. */
const NationalTeamRecord *ModernNational_Record(uint16_t team);
void ModernNational_Refresh(const ModernWorld *world);
unsigned ModernNational_SeedingStrength(const NationalTeamRecord *record);
int ModernNational_CanManage(uint16_t team);
int ModernNational_ProjectTeam(TeamFileEx *team);
unsigned ModernNational_PlayerAge(ModernWorld *world, const TeamFileEx *team, unsigned slot);
enum { MODERN_NATIONAL_SQUAD_SIZE = 20 };
/* Resolve current call-ups to their club player UUIDs before season rollover. */
void ModernNational_SnapshotCallups(ModernWorld *world,
    char ids[MODERN_NATIONAL_SQUAD_SIZE][MODERN_ID_LEN]);

void ModernNational_ReconcileCallups(ModernWorld *world,
    const char ids[MODERN_NATIONAL_SQUAD_SIZE][MODERN_ID_LEN]);

int ModernNational_PlayerEligible(const ModernRosterPlayer *player);
int ModernNational_PlayerCalledUp(uint16_t club, const PlayerFile *player, const char *clubName);
uint16_t ModernNational_CallUp(uint16_t club, uint16_t slot, const PlayerFile *player,
                               const char *clubName);

extern int ModernNational_StartPicker;

#endif
