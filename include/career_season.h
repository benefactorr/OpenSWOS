/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
/* career_season.h — Career season management, team list building, qualification. */
#pragma once
#include "game_types.h"

/* --- Random Team List Filling --- */
void Career_FillRandomTeamListExcludingSelected(uint16_t excludeCount,
                                                  uint8_t *table, uint8_t *compIds);

/* --- European Cup Qualification State --- */
void Career_PrepareEuropeanCupQualificationState(void);

/* --- Competition abbreviation (Chunk 145) --- */
uint8_t *GetCompetitionAbbrevStr(uint8_t *competition);

/* --- Chunk 161: Season opinion --- */
void ShowSeasonOpinion(void);

/* --- Career_UpdateLastNationalityCall (Chunk 170) --- */
uint16_t Career_UpdateLastNationalityCall(void);

/* --- Chunk 174: Season Review functions --- */
void SeasonFinished_ComputeSeasonReview(void);
void ShowChairmanOpinionAboutTheSeason(void);

/* --- Chunk 181: Contract Negotiation --- */
void BeginContractNegotiationSequence(DIYCompetitionEx *comp, TeamFileEx *team, TeamFileEx *opponent);

/* --- Chunk 182: Competition Season Processing --- */
void Career_ProcessSecondaryCompetitionForNextSeason(uint8_t *competition);
void Career_ProcessTertiaryCompetitionForNextSeason(uint8_t *competition);
void Career_ProcessCompetitionForNextSeason(uint16_t compIndex);

/* --- Chunk 185: Cup Pick List System --- */
void Career_SetCupPickList_ForSelectedRegion(uint16_t *teamList);

/* --- Chunk 186: Season Update Functions --- */
void Career_AdvanceToNextSeason_UpdateCompetitions(void);

#define CAREER_WINNER_SEASONS 35
#define CAREER_WINNERS_PER_SEASON 512

typedef struct CareerWinnerRecord {
    uint16_t teamId;
    uint8_t competitionId;
    uint8_t division;
    uint8_t kind;
} CareerWinnerRecord;
extern CareerWinnerRecord _careerWinners[CAREER_WINNER_SEASONS][CAREER_WINNERS_PER_SEASON];
extern uint16_t _careerWinnerCounts[CAREER_WINNER_SEASONS];
void CareerWinners_Clear(void);
void CareerWinners_RecordFinishedNationalSlots(void);
void CareerWinners_Record(uint16_t season, uint8_t competitionId,
                          uint8_t kind, uint8_t division, uint16_t teamId);

extern uint16_t CareerViewWorld_Active;
int CareerHistory_Active(void);
void Career_SeedInternationalCompetitions(void);
uint8_t Career_BackgroundWorldCupFlags(void);
void Career_OpenPastWinners(uint8_t id, uint8_t kind, uint8_t division);
