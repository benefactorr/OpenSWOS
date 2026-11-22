/* Restored Career+ winner history. The previous diagnostic source declared
 * these symbols but accidentally omitted their implementations, breaking
 * both the linker and the season-end history path.
 *
 * Kept separate from the large competition transition implementation so
 * source-only patches do not replace unrelated season code. */
#include "career_season.h"
#include "world.h"
#include "game_types.h"
#include "comp_identity.h"
#include <stdio.h>

extern uint16_t _gameType;

CareerWinnerRecord _careerWinners[CAREER_WINNER_SEASONS][CAREER_WINNERS_PER_SEASON];
uint16_t _careerWinnerCounts[CAREER_WINNER_SEASONS];

int CareerHistory_Active(void)
{
    return _gameType == 4;
}

void CareerWinners_Clear(void)
{
    for (unsigned season = 0; season < CAREER_WINNER_SEASONS; ++season)
        _careerWinnerCounts[season] = 0;
}

void CareerWinners_Record(uint16_t season, uint8_t competitionId,
                          uint8_t kind, uint8_t division, uint16_t teamId)
{
    if (season >= CAREER_WINNER_SEASONS || competitionId == 0xFF ||
        teamId == 0xFFFF || kind > 4)
        return;

    /* Past Winners has only two classes: league (0) and all cups (1). */
    kind = kind ? 1 : 0;
    unsigned count = _careerWinnerCounts[season];
    if (count > CAREER_WINNERS_PER_SEASON)
        count = CAREER_WINNERS_PER_SEASON;
    for (unsigned i = 0; i < count; ++i) {
        CareerWinnerRecord *r = &_careerWinners[season][i];
        if (r->competitionId == competitionId && r->kind == kind &&
            r->division == division) {
            r->teamId = teamId;
            return;
        }
    }
    if (count >= CAREER_WINNERS_PER_SEASON)
        return;
    _careerWinners[season][count] = (CareerWinnerRecord){
        teamId, competitionId, division, kind
    };
    _careerWinnerCounts[season] = (uint16_t)(count + 1);
}

/* Record finished national cups from the actual career slots before the next
 * season resets them. SaveJson also calls this so completed tournaments are
 * visible in Past Winners immediately after the final and survive reloading. */
extern uint16_t _currentSeasonShowing;
extern uint8_t *_currentCompetitionPtr;
extern uint8_t *_secondaryCompetitionPtr;
extern uint8_t *_tertiaryCompetitionPtr;
extern uint8_t *_quaternaryCompetitionPtr;
extern CompetitionSlotBuffer _primaryCompetitionBuf;
extern CompetitionSlotBuffer _competitionContextBuf;
extern CompetitionSlotBuffer _tertiaryCompetitionContextBuf;
extern CompetitionSlotBuffer _euroCupsCompetitionContextBuf;

static void record_national_slot(const uint8_t *competition,
                                 const CompetitionSlotBuffer *slot)
{
    if (!competition || !slot) return;
    const uint8_t id = comp_id(competition);
    if (id != COMPID_worldCup2 && !comp_is_continental_cup(id)) return;
    const DIYCompetition *state = &slot->tourn.header;
    const DIYCompetition *stage = (const DIYCompetition *)slot->stageSnapshot;
    /* A completed group stage is not the completed tournament. Require the
     * last knockout stage, as well as a completed state and a national ID. */
    if (!state->competitionFinished && !stage->competitionFinished) return;
    if ((state->numRounds > 0 && state->currentRoundIndex + 1 < state->numRounds) &&
        (stage->numRounds > 0 && stage->currentRoundIndex + 1 < stage->numRounds))
        return;
    /* Do not record a group-stage leader as the tournament champion. */
    if (state->hasGroups && stage->numTeamsInStage > 2 &&
        stage->currentRoundIndex + 1 < stage->numRounds) return;
    /* Prefer the actual completed final's result. A group-stage leader
     * can linger in the parent header after knockout stage transitions. */
    uint16_t winner = state->winnerTeam;
    if (stage->competitionFinished && stage->numTeamsInStage == 2 &&
        (stage->winnerTeam >> 8) >= 0x50u &&
        (stage->winnerTeam >> 8) <= 0x55u)
        winner = stage->winnerTeam;
    else if (winner == 0xFFFFu || (winner >> 8) < 0x50u ||
             (winner >> 8) > 0x55u)
        winner = stage->winnerTeam;
    if ((winner >> 8) < 0x50u || (winner >> 8) > 0x55u || winner == 0xFFFFu)
        return;
    if (_currentSeasonShowing >= CAREER_WINNER_SEASONS) return;
    CareerWinners_Record(_currentSeasonShowing, id, 1, 0, winner);
}

void CareerWinners_RecordFinishedNationalSlots(void)
{
    if (!CareerHistory_Active()) return;
    record_national_slot(_currentCompetitionPtr, &_primaryCompetitionBuf);
    record_national_slot(_secondaryCompetitionPtr, &_competitionContextBuf);
    record_national_slot(_tertiaryCompetitionPtr, &_tertiaryCompetitionContextBuf);
    record_national_slot(_quaternaryCompetitionPtr, &_euroCupsCompetitionContextBuf);
}
