/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
/* career_init.c — Career mode transfer market initialization.
 *
 * Chunk 61: 3 functions ported from SWOS2.s.
 */

#include "ai_career_pool_capacity.h"
#include "game_types.h"
#include "comp_identity.h"
#include <string.h>   /* memcpy in Rebuild_CupRecordNameSourcePointers */
#include <stdlib.h>   /* getenv: optional AI swap smoke test */
#include <stdio.h>    /* optional AI swap test log */
#include <time.h>     /* vary manual transfer seeds between clicks */
#include "endian_helpers.h"
#include "menu_system.h"
#include "input_text.h"
#include "save_format_json.h"
#include "swos_io.h"
#include "rnc.h"  /* RNC v1/v2 compression used by SWOS TEAM.NNN assets */
#include "random.h"
#include "string_utils.h"
#include "panic.h"
#include "auto_skip_debug.h"
#ifdef SWOS_INJECT_WC
#include <stdio.h>   /* fprintf — SWOS_INJECT_WC debug log only */
#endif

/* ---- External globals (all in SWOS2.s BSS) ---- */
extern uint16_t _numPlayersOnTransferMarket;
extern uint16_t _numForeignMarketPlayers2;
extern uint16_t _transferMarketRandOffset0;
extern uint16_t _transferMarketRandOffset1;
extern uint16_t _transferMarketRandOffset2;
extern uint16_t _transferMarketPhase;
extern uint16_t _timeToNegotiate;
extern uint16_t _numOffers;
extern uint16_t _transferPoolSlotsUsed;
extern uint16_t _offerSwapSlotsUsed;
extern uint16_t _lastOfferPrimaryPlayerOrdinal;
extern uint16_t _offerNegotiationAttemptCount;
extern uint16_t _careerSeasonsCompleted;
#include "career_params.h"
extern uint16_t _currentSeasonShowing;
extern uint16_t _seasonTransferQuota;
extern uint16_t _pendingForeignPlayerCount;
extern uint16_t _offerSwapSlotLimit;
extern uint16_t _transferPoolSlotLimit;
extern uint16_t _homeMarketUpdateBacklog;
extern uint16_t _foreignMarketUpdateBacklog;
extern uint16_t _offerExpiryIterationsRemaining;
extern uint16_t _removedOfferPlayersFromSelectedTeamCount;
extern uint16_t _removedOfferPlayersFromOtherTeamsCount;

/* careerCupPickList: 5-entry array of COMPETITION POINTERS (national-coach
 * cup pick list). Pointer-sized entries — was extern'd uint32_t (truncating
 * the 64-bit pointers); now a standalone void *[5] (see swos_data_typed_stubs.c). */
extern void *_careerCupPickList[];
extern uint8_t _invitationTournamentCopy[];
extern uint8_t _tourCopy[];

/* Globals for manager setup menu (Chunk 110) */
extern MenuEntry _menuEntries[];
extern uint16_t _coachOrPlayer;
extern uint16_t _mrOrMs;
extern uint8_t _managerSexBuffer[];
extern uint8_t _aMr[];           /* "MR" */
extern uint8_t _aMs[];           /* "MS" */

/* Globals for Career_InitializeSelectedTeamAndStartingBalance (Chunk 170) */
extern int32_t  _startingBalance;
extern uint16_t _teamNoOfferAccepted;
extern uint16_t _stayingInOldClub;
extern uint16_t _careerTeamLeague;
extern uint16_t _selTeamNumber;
extern TeamFileEx _careerTeam;
extern uint8_t  _currentMatchPlayers[];
extern uint16_t _teamsLoaded;
extern int32_t  _newBalance;
extern uint16_t _careerNumPlayers;
extern uint16_t _sensisoftPlcSponsorshipFactor;
extern uint16_t _additionalInvestments;
extern uint16_t _isNationalSelector;

/* ================================================================
 * Career_InitTransferMarketState  (SWOS2.s line 72836)
 *
 * Clears transfer market counters and generates three random
 * offsets (0..15) used during transfer market initialization.
 * ================================================================ */
__attribute__((used, externally_visible))
void Career_InitTransferMarketState(void)
{
    _numPlayersOnTransferMarket = 0;
    _numForeignMarketPlayers2 = 0;
    _transferMarketRandOffset1 = Rand2() & 0xF;
    _transferMarketRandOffset2 = Rand2() & 0xF;
    _transferMarketPhase = 0;
    _transferMarketRandOffset0 = Rand2() & 0xF;
}

/* ================================================================
 * Career_InitSeasonTransferMarketState  (SWOS2.s line 72853)
 *
 * Initializes per-season transfer market state: negotiation time,
 * offer counts, pool quotas, and backlog counters.
 *
 * Season transfer quota formula:
 *   numerator = 432 - numForeignMarketPlayers2 - (7 - seasons) * 16
 *   divisor   = 20 - currentSeasonShowing
 *   quota     = (uint32_t)(int32_t)(int16_t)numerator / divisor
 * ================================================================ */
__attribute__((used, externally_visible))
void Career_InitSeasonTransferMarketState(void)
{
    _timeToNegotiate = 6;
    _numOffers = 0;
    _transferPoolSlotsUsed = 0;
    _offerSwapSlotsUsed = 0;
    _lastOfferPrimaryPlayerOrdinal = 0xFFFF;
    _offerNegotiationAttemptCount = 0;

    /* The original 432-entry career-pool budget is no longer meaningful:
     * manual AI transfers use a separate expanded (50,000) record pool.
     * Once the career pool exceeded 432, the old compatibility clamp
     * set quota=0 and therefore offerSwapSlotLimit=0. This stopped ALL
     * ordinary transfer bids, even though thousands of records remained.
     *
     * Allocate a modest, stable annual budget for normal transfer-market
     * processing. It resets every season, independently of AI exchanges.
     * The replacement pool capacity is still checked before appending.
     */
    enum { NORMAL_MARKET_SEASON_QUOTA = 256u };
    _seasonTransferQuota = NORMAL_MARKET_SEASON_QUOTA;
    _pendingForeignPlayerCount = 0;
    _offerSwapSlotLimit = NORMAL_MARKET_SEASON_QUOTA / 2;
    _transferPoolSlotLimit = NORMAL_MARKET_SEASON_QUOTA / 2;

    _homeMarketUpdateBacklog = 0;
    _foreignMarketUpdateBacklog = 0;
    _offerExpiryIterationsRemaining = 0;
    _removedOfferPlayersFromSelectedTeamCount = 0;
    _removedOfferPlayersFromOtherTeamsCount = 0;
}

/* ================================================================
 * Career_RandomizeInvitationTournamentCup  (SWOS2.s line 35893)
 *
 * Scans the 5-entry careerCupPickList for entries pointing to
 * invitationTournamentCopy. For each such entry, 50% chance to
 * replace it with tourCopy (randomized via Rand).
 * ================================================================ */
__attribute__((used, externally_visible))
void Career_RandomizeInvitationTournamentCup(void)
{
    void **pickList = _careerCupPickList;
    int16_t i;

    for (i = 4; i >= 0; i--) {
        void *p = pickList[i];
        /* Entries are competition-copy pointers or the table sentinels 0 / -1 / -2;
         * only dereference real pointers for the id check (the original compared the
         * raw pointer, which was sentinel-safe). The replacement stores the actual
         * tour competition object, so it keeps referencing &_tourCopy. */
        if (p != 0 && p != (void *)(uintptr_t)-1 && p != (void *)(uintptr_t)-2 &&
            comp_id(p) == COMPID_invitationTournament) {
            if (Rand() & 1)
                pickList[i] = (void *)_tourCopy;
        }
    }
}

/* ================================================================
 * Manager setup menu callbacks (Chunk 110)
 * Asm: SetPlayerCoachFieldColor, CheckSex, ChangeSex,
 *      SetName, SetSurname, CoachOrPlayerCoach
 * ================================================================ */

/* SetPlayerCoachFieldColor: update background color palette index
 * for menu entry 9 based on coachOrPlayer toggle.
 * The low nibble of the bg color word holds the color palette index. */
__attribute__((used, externally_visible))
void SetPlayerCoachFieldColor(void)
{
    MenuEntry *e = &_menuEntries[9];
    e->bgColor = (e->bgColor & 0xFFF0) | 13; /* BRIGHT_BLUE_TO_BLUE_13 */
    if (_coachOrPlayer != 0)
        e->bgColor = (e->bgColor & 0xFFF0) | 11; /* BLUE_TO_PURPLE_11 */
}

/* CheckSex: copy "MR" or "MS" to managerSexBuffer based on mrOrMs toggle */
__attribute__((used, externally_visible))
void CheckSex(void)
{
    uint8_t *src = _aMr;
    if (_mrOrMs != 0)
        src = _aMs;
    StringCopy(src, _managerSexBuffer);
}

/* ChangeSex: toggle mrOrMs, update sex string, redraw current menu item.
 * entry = a5 from menu system (current MenuEntry). */
__attribute__((used, externally_visible))
void ChangeSex(MenuEntry *entry)
{
    _mrOrMs ^= 1;
    CheckSex();
    CallDrawMenuItem(entry);
}

/* SetName: start text input for first name field (max 9 chars).
 * entry = a5 from menu system; InputText reads entry->fg itself. */
__attribute__((used, externally_visible))
void SetName(MenuEntry *entry)
{
    InputText(9, (uint8_t *)entry);
}

/* SetSurname: start text input for surname field (max 13 chars).
 * entry = a5 from menu system; InputText reads entry->fg itself. */
__attribute__((used, externally_visible))
void SetSurname(MenuEntry *entry)
{
    InputText(13, (uint8_t *)entry);
}

/* CoachOrPlayerCoach: toggle coach/player-coach mode, update color, redraw.
 * entry = a5 from menu system (current MenuEntry). */
__attribute__((used, externally_visible))
void CoachOrPlayerCoach(MenuEntry *entry)
{
    _coachOrPlayer ^= 1;
    SetPlayerCoachFieldColor();
    CallDrawMenuItem(entry);
}

/* ================================================================
 * Chunk 148: ChooseTeamAndStartCareer, LoadLiverpool
 * ================================================================ */

extern uint8_t _contestMenu[];
extern void ActivateMenu(uint8_t *menu);
extern void SetExitMenuFlag(void);

/* Forward declarations for Chunk 197 functions (defined later in this file) */
void InitCareer(uint16_t teamNo);
void ContinueCareerNextSeason(void);
void InitializeNewSeason(void);
void DIY_SelectCompetitionSlot4FromCurrentDivision(void);
uint16_t Career_SelectEuroCupSlotIfTeamParticipates(uint8_t *cupCopy, uint16_t euroCupIndex);

extern uint16_t _selectedTeamCountryNumber;
extern TeamFileEx *_selectedTeamPtr;
void SelectTeamToManage(void);

/* ChooseTeamAndStartCareer: let user pick a team, then start career.
 * Called as menu onSelect callback. */
__attribute__((used, externally_visible))
void ChooseTeamAndStartCareer(void)
{
    SelectTeamToManage();
    if (_selectedTeamPtr == 0) {
        SetExitMenuFlag();
        return;
    }
    InitCareer(_selectedTeamCountryNumber);
    ActivateMenu(_contestMenu);
}

/* LoadLiverpool: debug/cheat — start career with Liverpool (team 0x811).
 * Called as menu onSelect callback. */
__attribute__((used, externally_visible))
void LoadLiverpool(void)
{
    _coachOrPlayer = 1;
    InitCareer(0x811);
    ActivateMenu(_contestMenu);
}

/* ================================================================
 * Chunk 149: ContinueCareer  (SWOS2.s line 55140)
 *
 * If season == 19, show retirement message + reset career variables.
 * Otherwise advance to next season and re-activate contest menu.
 * ================================================================ */

extern void ShowRetirementMessage(void);
extern void InitCareerVariables(uint16_t gameTypeParam);

__attribute__((used, externally_visible))
void ContinueCareer(void)
{
    if (_currentSeasonShowing == _careerParams.num_seasons - 1) {
        ShowRetirementMessage();
        InitCareerVariables(0);
        SetExitMenuFlag();
    } else {
        ContinueCareerNextSeason();
        ActivateMenu(_contestMenu);
    }
}

/* ================================================================
 * Chunk 151: InitCompetitionTeamSelection functions
 *
 * These initialize team selection state for career competition setup.
 * They all follow a similar pattern:
 *   1. Clear careerFileBuffer
 *   2. Call LoadSomeEuroCup
 *   3. Clear numSelectedTeams
 *   4. Call Career_InitCompetitionTeamSelectionFrom*Spec
 *   5. Read numNeededTeams from the competition record
 *   6. Return 0
 * ================================================================ */
extern uint16_t _numSelectedTeams;
extern uint16_t _careerFileBuffer;
extern uint16_t _numNeededTeams;

extern void LoadSomeEuroCup(uint8_t *whichCup);
extern void Career_InitCompetitionTeamSelectionFromTableEntrySpec(uint8_t *base, uint16_t idx);
extern void Career_InitCompetitionTeamSelectionFromTableHeaderSpec(uint8_t *base);
extern void Career_InitCompetitionTeamSelectionFromAltTableHeaderSpec(uint8_t *base);

__attribute__((used, externally_visible))
uint16_t InitCompetitionTeamSelectionFromTableEntry(uint16_t idx, uint8_t *base)
{
    _careerFileBuffer = 0;
    LoadSomeEuroCup(base);
    _numSelectedTeams = 0;
    Career_InitCompetitionTeamSelectionFromTableEntrySpec(base, idx);

    /* numNeededTeams = record byte at offset 0xD in idx'th 6-byte entry */
    uint16_t offset = idx * 6;
    _numNeededTeams = (int8_t)base[0xD + offset];
    return 0;
}

__attribute__((used, externally_visible))
uint16_t InitCompetitionTeamSelectionFromTableHeader(uint8_t *base)
{
    _careerFileBuffer = 0;
    LoadSomeEuroCup(base);
    _numSelectedTeams = 0;
    Career_InitCompetitionTeamSelectionFromTableHeaderSpec(base);

    _numNeededTeams = (int8_t)base[0xA];
    return 0;
}

__attribute__((used, externally_visible))
uint16_t InitCompetitionTeamSelectionFromAltTableHeader(uint8_t *base)
{
    _careerFileBuffer = 0;
    LoadSomeEuroCup(base);
    _numSelectedTeams = 0;
    Career_InitCompetitionTeamSelectionFromAltTableHeaderSpec(base);

    _numNeededTeams = (int8_t)base[0xF];
    return 0;
}

/* ================================================================
 * Career_InitializeSelectedTeamAndStartingBalance (SWOS2.s line 34621)
 *
 * Sets up the career team data by copying the selected team's file
 * data to both careerTeam and selTeamsPtr buffers, sets the
 * coach/player control byte, copies player order to currentMatchPlayers,
 * and initializes financial globals.
 *
 * Asm: d0=teamNumber (uint16), d1=startingBalance (int32)
 * ================================================================ */
extern int16_t GetTeamLeague(uint16_t teamNumber);
extern TeamFileEx *GetTeamPointer(uint16_t teamId);

__attribute__((used, externally_visible))
void Career_InitializeSelectedTeamAndStartingBalance(uint16_t teamNumber, int32_t balance)
{
    _startingBalance = balance;
    _teamNoOfferAccepted = 0xFFFF;
    _stayingInOldClub = 0xFFFF;
    _numSelectedTeams = 0;

    /* Get team league for career tracking */
    _careerTeamLeague = (uint16_t)GetTeamLeague(teamNumber);

    /* Get pointer to team file data and copy to both buffers */
    TeamFileEx *teamPtr = GetTeamPointer(teamNumber);
    _selTeamNumber = teamNumber;

    /* Copy 342 words (684 bytes = one TeamFileEx) to both careerTeam and selTeamsPtr */
    uint16_t *src = (uint16_t *)teamPtr;
    uint16_t *dst1 = (uint16_t *)&_careerTeam;
    uint16_t *dst2 = (uint16_t *)&_selTeamsPtr;
    for (int16_t i = 341; i >= 0; i--) {
        *dst1++ = *src;
        *dst2++ = *src++;
    }

    _numSelectedTeams = 1;

    /* Set teamNameControl byte (offset 4) based on coach-or-player setting:
     * 2 = coach only, 3 = player-coach */
    uint8_t controlByte = (_coachOrPlayer != 0) ? 2 : 3;
    _careerTeam.team.teamControls = controlByte;
    _selectedTeams->team.teamControls = controlByte;

    /* Copy 16 bytes of playerNumbers (TeamFile.playerNumbers) to currentMatchPlayers */
    teamPtr = GetTeamPointer(_selTeamNumber);
    for (int16_t i = 15; i >= 0; i--) {
        _currentMatchPlayers[i] = teamPtr->team.playerNumbers[i];
    }

    _careerNumPlayers = 16;
    _teamsLoaded = 0;
    _newBalance = 0;
    _sensisoftPlcSponsorshipFactor = 0;
    _additionalInvestments = 0;
}

/* ================================================================
 * Career_ContinueToNextSeasonInitTeamAndFunds (SWOS2.s line 72514)
 *
 * Prepares for a new career season: clears transfer market, increments
 * season counter, optionally rebuilds foreign market pool, applies a
 * random ±jitter (80-104%) to the base funds, then initializes the
 * team with the adjusted balance.
 *
 * Asm: d0=teamNumber (uint16), d1=baseFunds (uint32)
 * ================================================================ */
extern void Career_BuildForeignMarketPoolFromSelectedTeam(void);
extern uint16_t Rand2(void);

__attribute__((used, externally_visible))
void Career_ContinueToNextSeasonInitTeamAndFunds(uint16_t teamNumber, uint32_t baseFunds)
{
    _numPlayersOnTransferMarket = 0;
    _careerSeasonsCompleted++;

    if (_isNationalSelector == 0) {
        Career_BuildForeignMarketPoolFromSelectedTeam();
    }

    /* Apply random jitter to funds: 80-104% of original
     * Formula: ((baseFunds / 1000) * (80 + rand25pct)) / 100 * 1000
     * where rand25pct = (Rand2() * 25) >> 8 = 0..24 */
    uint16_t fundsDiv = (uint16_t)(baseFunds / 1000);
    uint16_t rand = Rand2();
    uint16_t jitterPct = ((rand * 25) >> 8) + 80;
    uint32_t adjusted = (uint32_t)fundsDiv * (uint32_t)jitterPct;
    uint16_t adjustedDiv = (uint16_t)(adjusted / 100);
    uint32_t finalFunds = (uint32_t)adjustedDiv * 1000;

    Career_InitializeSelectedTeamAndStartingBalance(teamNumber, (int32_t)finalFunds);
}

/* ================================================================
 * Chunk 197: InitCareer, ContinueCareerNextSeason,
 *   Career_SelectEuroCupSlotIfTeamParticipates,
 *   DIY_SelectCompetitionSlot4FromCurrentDivision,
 *   InitializeNewSeason, InitNewSeason
 * ================================================================ */

/* --- External globals (Chunk 197) --- */
extern uint16_t _worldCupHostTeam;
extern uint8_t  _diyLeagueSeasonStateTable[];
extern uint16_t _sameNationalCallFlag;
extern uint16_t _boardConfidenceHistory0;
extern uint16_t _boardConfidenceHistory1;
extern uint16_t _boardConfidenceHistory2;
extern uint16_t _boardConfidenceHistory3;
extern uint16_t _boardConfidenceScore;
extern uint16_t _gamesPlayed;
extern uint16_t _numWins;
extern uint16_t _numDrawn;
extern uint16_t _numLost;
extern uint16_t _goalsScored;
extern uint16_t _goalsTaken;
extern uint16_t _careerReputationPoints;
extern uint16_t _winCupBonusCount;
extern uint16_t _minorTrophyBonusCount;
extern uint16_t _majorTrophyBonusCount;
extern uint16_t _playPromotedAnimation;
extern uint16_t _playRelegatedAnimation;
extern uint16_t _previousBoardConfidence;
extern uint16_t _playWinCupAnimation;
extern uint16_t _minorTrophyBonusActive;
extern uint16_t _leagueTitleBonusActive;
extern uint16_t _majorTrophyBonusActive;
extern uint16_t _cupBonus1Active;
extern uint16_t _cupBonus2Active;
extern uint16_t _wonWorldCup;
extern uint16_t _wonContinentalCup;
extern uint16_t _qualifyingStage1PendingFlag0;
extern uint16_t _qualifyingStage1PendingFlag1;
extern uint16_t _qualifyingStage2PendingFlag0;
extern uint16_t _qualifyingStage2PendingFlag1;
extern uint16_t _wonInvitationTournament;

extern uint8_t  _euroCup[];
extern uint8_t  _cupWinnersCup[];
extern uint8_t  _uefaCup[];

/* careerContests table — typed CompetitionCareer[] entries
 * {src_ptr, dest_ptr, byte_count} terminated by `competition ==
 * (CompetitionCup *)0xFFFFFFFFu`. Was `extern uint8_t careerContests[]`
 * with byte-stride walks (10 B per entry); on 64-bit each entry is
 * 18 B so consumers iterate the typed array instead. */
extern CompetitionCareer _careerContests[];

/* primaryCompSlotStateA/B/C migrated to primaryCompetitionBuf.tourn.header.slotState* (slot-0 untangle, S6) */
/* secondaryCompSlotStateA/B/C migrated to competitionContextBuf.tourn.header.slotState* (slot-1 untangle, S4) */
/* tertiaryCompSlotStateA/B/C migrated to tertiaryCompetitionContextBuf.tourn.header.slotState* (slot-2 untangle, S3) */
/* quaternaryCompSlotStateA/B/C migrated to euroCupsCompetitionContextBuf.tourn.header.slotState* (slot-3 untangle, S5) */
extern uint8_t  _seasonPlaying;

extern uint16_t _poolplyrLoaded;

extern void DIY_LoadLeagueScratchFromTable(void);
extern uint32_t CalculateStartingBalance(uint16_t teamNumber);

/* --- InitializeNewSeason externs --- */
extern uint16_t _selTeamSelectedPlayerPosition;
extern uint8_t  _careerUnavailablePlayerList[];
extern uint16_t _retired;
extern uint16_t _youreSacked;
extern uint16_t _competitionSlotUpdatedFlag;
extern uint16_t _boardConfidenceMod0;
extern uint16_t _boardConfidenceMod1;
extern uint16_t _boardConfidenceMod2;
extern uint16_t _boardConfidenceMod3;
extern uint16_t _boardConfidenceValue0;
extern uint16_t _boardConfidenceValue1;
extern uint16_t _boardConfidenceMod4;
extern uint16_t _boardConfidenceMod5;
extern uint16_t _boardConfidenceMod6;
extern uint16_t _boardConfidenceMod7;
extern uint16_t _boardConfidenceMod8;
extern uint16_t _boardConfidenceMod9;
extern uint16_t _currentDivisionIndex;
extern uint16_t _contractNegotiationActive;
extern uint16_t _removedPlayerStatAccum;
extern uint16_t _teamSelectionKeyCacheEnd;
extern uint16_t _isCareer;
extern uint16_t _careerMatchSimulatedFlag;
extern uint16_t _numNewJobOffers;
extern uint16_t _numJobOffers;
extern uint16_t _careerTeamLostMatch;
extern volatile uint16_t _currentMessage;
extern uint8_t  _careerLastMatchIndexByCompetitionSlot[];
extern uint8_t *_currentCompetitionPtr;
extern uint8_t *_secondaryCompetitionPtr;
extern uint8_t *_tertiaryCompetitionPtr;
extern uint8_t *_quaternaryCompetitionPtr;
extern uint8_t *_diyCompetitionPtr;
extern CompetitionSlotBuffer _primaryCompetitionBuf;          /* slot-0 untangle (S6): typed buffer */
extern CompetitionSlotBuffer _competitionContextBuf;          /* slot-1 untangle (S4): typed buffer */
extern CompetitionSlotBuffer _tertiaryCompetitionContextBuf;   /* slot-2 untangle (S3): typed buffer */
extern CompetitionSlotBuffer _euroCupsCompetitionContextBuf;   /* slot-3 untangle (S5): typed buffer */
extern CompetitionSlotBuffer _diyCompetitionContextBuf;   /* slot-4 untangle (S2): typed buffer */
extern uint8_t *_careerCompetitionSlotPtrTable[];
extern uint16_t _teamSelectionKeyCacheCount;
extern uint16_t _teamSelectionKeyCacheIndex;
extern uint16_t _teamSelectionKeyCacheOffset;
extern uint16_t _careerSlotProcessingInProgress;
extern uint16_t _lastNationalityCall;
extern void    *_careerTeamFlagsByRegionTable[];  /* 6-entry pointer table; native width on 32 and 64-bit. Storage is `void *[6]` in swos_amiga_ptr_imports.c. */
extern uint8_t  _someTeamFlags;
extern uint8_t  _nationalTeamNationality;
extern uint8_t  _nationalTeamFileId;
extern uint16_t _worldCupQualifyingTeam;
extern uint16_t _euroChampionshipQualTeam;
extern uint16_t _seasonCycleNationalFlagsA[];
extern uint16_t _seasonCycleNationalFlagsB[];
extern uint8_t *_primaryCompStringOffset;
extern uint8_t *_secondaryCompStringOffset;
extern uint8_t *_tertiaryCompStringOffset;
extern uint8_t *_quaternaryCompStringOffset;
extern uint8_t  _euroCupCopy[];
extern uint8_t  _cupWinnersCupCopy[];
extern uint8_t  _uefaCupCopy[];
extern void *_competitionsTable[];
/* careerCupPickList: declared at top of file (line 39) */
extern uint16_t _euroCupSlotIndex;
extern uint32_t *_diyCompetitionTablePtr;
extern uint8_t  _careerJobOfferPhaseBias;
extern uint8_t  _viewRandomPhase;
extern uint8_t  _lastFinancialWeekIndex;
extern uint16_t _wageScalingDivisor;
extern uint16_t _leagueFirstRoundNumber;
extern uint16_t _seasonFinished;
extern uint16_t _careerFinished;
extern uint8_t  _managerNameBuffer[];
extern uint8_t  _managerSurnameBuffer[];
extern uint16_t _seasonNumberOfSquads;
extern uint16_t _seasonSquadTeamNumbers[];
extern uint16_t _careerLoadedFlag;
extern uint8_t  _teamSelectionKeyCache[];
extern uint32_t _teamFundsAccepted;
extern uint8_t  _nationalTeamFileCode;

extern uint8_t *GetCurrentSeasonPointer(void);
extern void DIY_SaveLeagueScratchToTable(void);
extern TeamFileEx *VerifyTeamLoaded(uint16_t teamNo);
extern void InitTeams(void);
extern void FillForeignMarketPlayers(void);
extern void FillCareerTeamPlayers(void);
extern void Career_InitializeNewSeasonFinances(void);
extern void InitializeCareerSeasonFinancials(void);
extern void ShowEnjoyLastSeasonMessage(void);
extern void ShowWishYouSuccessMessage(void);
extern void ShowLastSeasonMessage(void);
extern void ShowGreetingMessage(void);
extern void ShowLeavingMessage(void);
extern void TrainingMatch_AddTrialPlayersForNewSeason(void);
extern void Career_InitInternationalSquadAndLookupTablesForNewSeason(TeamFileEx *teamPtr);
extern void Career_SetCupPickList_ForSelectedRegion(uint16_t *teamList);
extern uint16_t Career_UpdateLastNationalityCall(void);
extern void InitializeNewSeason_BuildTeamBudgetRankTable(void);
extern void Career_SetCurrentCompetitionSlot(uint8_t *comp, uint16_t slot, uint16_t divisionIndex, uint16_t hasSelectedTeams);
extern void ShowChairmanOpinionAboutTheSeason(void);
extern void InitializeStartingBalanceForNewSeason(void);
extern void Career_AdvanceToNextSeason_UpdateCompetitions(void);
extern uint32_t GetNationalityAndTeamFile(uint16_t teamNo);

/* SWOS AI Transfers Prototype 1: an opt-in, new-career-only smoke test.
 * Enable via SWOS_AI_SWAP_TEST=1.  It swaps two starting goalkeepers from
 * two AI clubs and records both changes in the existing JSON-saved career
 * replacement pool.  It does not modify the original TEAM.* files.
 * This is deliberately NOT the full-world transfer algorithm.
 */
static void SWOS_AISwapSmokeTest(void)
{
    const char *enabled = getenv("SWOS_AI_SWAP_TEST");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    extern TeamFileEx *GetTeamPointer(uint16_t teamId);
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _lastLoadedExtNumber;
    extern uint8_t _viewRandomPhase;

    /* Country 1 first, country 2 if the manager controls one of the
     * candidate clubs.  Ordinal 0 is goalkeeper at both clubs. */
    uint16_t clubA = 0x0100, clubB = 0x0101;
    if (_selTeamNumber == clubA || _selTeamNumber == clubB) {
        clubA = 0x0200;
        clubB = 0x0201;
    }
    if (_selTeamNumber == clubA || _selTeamNumber == clubB)
        return;
    if (_numForeignMarketPlayers2 > SWOS_AI_CAREER_POOL_CAPACITY - 2)
        return;

    /* GetTeamPointer can reuse a scratch TEAM-file buffer: copy the first
     * player before asking for the second club. */
    TeamFileEx *teamA = GetTeamPointer(clubA);
    if (!teamA || teamA->team.countryNumber != (clubA >> 8) ||
        teamA->team.teamOrdinal != (clubA & 255))
        return;
    PlayerFile playerA = teamA->players[0];

    TeamFileEx *teamB = GetTeamPointer(clubB);
    if (!teamB || teamB->team.countryNumber != (clubB >> 8) ||
        teamB->team.teamOrdinal != (clubB & 255))
        return;
    PlayerFile playerB = teamB->players[0];

    /* Both must be goalkeepers, otherwise leave the career untouched. */
    if ((playerA.positionAndFace & 0xE0u) != 0 ||
        (playerB.positionAndFace & 0xE0u) != 0)
        return;

    /* Duplicate slot overrides would be ambiguous, so abort if present. */
    for (uint16_t i = 0; i < _numForeignMarketPlayers2; i++) {
        const MarketPlayer *e = &_careerForeignMarketPlayers[i];
        const uint8_t *id = (const uint8_t *)&e->teamId;
        uint16_t teamId = ((uint16_t)id[0] << 8) | id[1];
        if ((teamId == clubA || teamId == clubB) && e->playerOrdinal == 0)
            return;
    }

    MarketPlayer *a = &_careerForeignMarketPlayers[_numForeignMarketPlayers2];
    MarketPlayer *b = &_careerForeignMarketPlayers[_numForeignMarketPlayers2 + 1];
    ((uint8_t *)&a->teamId)[0] = (uint8_t)(clubA >> 8);
    ((uint8_t *)&a->teamId)[1] = (uint8_t)clubA;
    a->playerOrdinal = 0;
    a->padding = (int8_t)_viewRandomPhase;
    a->player = playerB;

    ((uint8_t *)&b->teamId)[0] = (uint8_t)(clubB >> 8);
    ((uint8_t *)&b->teamId)[1] = (uint8_t)clubB;
    b->playerOrdinal = 0;
    b->padding = (int8_t)_viewRandomPhase;
    b->player = playerA;

    _numForeignMarketPlayers2 += 2;
    _pendingForeignPlayerCount += 2;
    _lastLoadedExtNumber = 255;

    /* Update any live cached club as well as the serialized overrides.
     * For scratch-loaded clubs the usual patch routine may already apply. */
    teamA = GetTeamPointer(clubA);
    if (teamA) teamA->players[0] = playerB;
    teamB = GetTeamPointer(clubB);
    if (teamB) teamB->players[0] = playerA;

    fprintf(stderr, "[SWOS AI P1] Swapped goalkeepers: %04X/%s <-> %04X/%s; overrides=%u\n",
            clubA, (char *)playerA.playerName, clubB, (char *)playerB.playerName,
            (unsigned)_numForeignMarketPlayers2);
}

/* Prototype 2.2: log directly to a file, because Windows GUI builds may
 * not inherit a visible stderr console even when launched from MSYS2. */
#define SWOS_AI_LOG(...) do { \
    FILE *swos_ai_file = fopen("swos_ai_debug.log", "a"); \
    if (swos_ai_file) { \
        fprintf(swos_ai_file, __VA_ARGS__); \
        fclose(swos_ai_file); \
    } \
} while (0)

/* Prototype 2.5: goalkeeper exchanges to exercise expanded replacement storage.
 * Enabled only for NEW careers with SWOS_AI_STRESS_TEST=1. Unlike P1,
 * this is a pool-capacity stress test, not an AI transfer policy.
 * Clubs are selected deterministically from existing TEAM records.
 * This is one goalkeeper per club, so the maximum record count is bounded
 * by the number of eligible clubs, regardless of the requested target.
 */
static void SWOS_AIStressTest(void)
{
    const char *enabled = getenv("SWOS_AI_STRESS_TEST");
    SWOS_AI_LOG("[SWOS AI P2.5] stress function called; flag=%s\n",
                enabled ? enabled : "(not set)");
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    extern int16_t LoadTeamFile(uint16_t extNumber);
    extern uint8_t _teamFileBuffer[];
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _lastLoadedExtNumber;
    extern uint8_t _viewRandomPhase;

    /* Diagnostic target is limited by storage capacity and actual available clubs. */
    uint16_t target = 20;
    const char *sizeEnv = getenv("SWOS_AI_STRESS_RECORDS");
    if (sizeEnv && *sizeEnv) {
        char *endptr = NULL;
        long requested = strtol(sizeEnv, &endptr, 10);
        if (endptr != sizeEnv && *endptr == '\0' && requested >= 2 && requested <= SWOS_AI_CAREER_POOL_CAPACITY)
            target = (uint16_t)(requested & ~1L);
    }

    uint16_t added = 0;
    uint16_t heldId = 0;
    PlayerFile heldPlayer;
    int held = 0;

    SWOS_AI_LOG("[SWOS AI P2.5] starting bounded test, target=%u\n", (unsigned)target);
    fprintf(stderr, "[SWOS AI P2.1] starting bounded test, target=%u\n", (unsigned)target);
    fflush(stderr);

    for (uint16_t country = 1; country <= 73 && added < target; country++) {
        /* TEAM numbers are not contiguous. The original supplied SWOS 96/97
         * database has gaps at these country codes. Calling LoadTeamFile on a
         * missing entry invokes PanicHaltLoop; do NOT use fopen relative to
         * the process working directory because the assets can be JSON-backed. */
        if (country == 9 || (country >= 52 && country <= 54) ||
            country == 56 || country == 58 || country == 61 || country == 63) {
            SWOS_AI_LOG("[SWOS AI P2.5] skip known missing country=%u\n", (unsigned)country);
            continue;
        }
        SWOS_AI_LOG("[SWOS AI P2.5] about to load country=%u\n", (unsigned)country);
        if (LoadTeamFile(country) != 0) {
            fprintf(stderr, "[SWOS AI P2.1] load country %u failed\n", (unsigned)country);
            continue;
        }
        uint16_t count;
        memcpy(&count, _teamFileBuffer, sizeof(count));
        SWOS_AI_LOG("[SWOS AI P2.5] country=%u team-count=%u inserted=%u\n",
                    (unsigned)country, (unsigned)count, (unsigned)added);
        if (count == 0 || count > 93) {
            fprintf(stderr, "[SWOS AI P2.1] suspicious team count=%u for country=%u\n",
                    (unsigned)count, (unsigned)country);
            continue;
        }
        fprintf(stderr, "[SWOS AI P2.1] country=%u clubs=%u, inserted=%u\n",
                (unsigned)country, (unsigned)count, (unsigned)added);
        fflush(stderr);
        const TeamFileEx *teams = (const TeamFileEx *)(_teamFileBuffer + 2);
        for (uint16_t i = 0; i < count && added < target; i++) {
            SWOS_AI_LOG("[SWOS AI P2.5] club BEGIN country=%u index=%u inserted=%u\n",
                        (unsigned)country, (unsigned)i, (unsigned)added);
            const TeamFileEx *team = &teams[i];
            uint8_t actualCountry = team->team.countryNumber;
            uint8_t ordinal = team->team.teamOrdinal;
            SWOS_AI_LOG("[SWOS AI P2.5] club header country=%u index=%u actualCountry=%u ordinal=%u\n",
                        (unsigned)country, (unsigned)i,
                        (unsigned)actualCountry, (unsigned)ordinal);
            if (actualCountry != country) continue;
            uint16_t teamId = (uint16_t)((country << 8) | ordinal);
            if (teamId == _selTeamNumber ||
                _numForeignMarketPlayers2 > SWOS_AI_CAREER_POOL_CAPACITY - 2)
                continue;
            PlayerFile goalkeeper = team->players[0];
            if ((goalkeeper.positionAndFace & 0xE0u) != 0)
                continue;
            int used = 0;
            for (uint16_t k = 0; k < _numForeignMarketPlayers2; k++) {
                const MarketPlayer *entry = &_careerForeignMarketPlayers[k];
                const uint8_t *bytes = (const uint8_t *)&entry->teamId;
                if (bytes[0] == actualCountry && bytes[1] == ordinal &&
                    entry->playerOrdinal == 0) { used = 1; break; }
            }
            if (used) continue;
            if (!held) {
                heldId = teamId;
                heldPlayer = goalkeeper;
                held = 1;
                continue;
            }
            MarketPlayer *a = &_careerForeignMarketPlayers[_numForeignMarketPlayers2];
            MarketPlayer *b = a + 1;
            ((uint8_t *)&a->teamId)[0] = (uint8_t)(heldId >> 8);
            ((uint8_t *)&a->teamId)[1] = (uint8_t)heldId;
            a->playerOrdinal = 0;
            a->padding = (int8_t)_viewRandomPhase;
            a->player = goalkeeper;
            ((uint8_t *)&b->teamId)[0] = actualCountry;
            ((uint8_t *)&b->teamId)[1] = ordinal;
            b->playerOrdinal = 0;
            b->padding = (int8_t)_viewRandomPhase;
            b->player = heldPlayer;
            SWOS_AI_LOG("[SWOS AI P2.5] append pair country=%u index=%u oldCount=%u\n",
                        (unsigned)country, (unsigned)i,
                        (unsigned)_numForeignMarketPlayers2);
            _numForeignMarketPlayers2 += 2;
            _pendingForeignPlayerCount += 2;
            _lastLoadedExtNumber = 255;
            added += 2;
            held = 0;
        }
    }
    SWOS_AI_LOG("[SWOS AI P2.5] complete inserted=%u total=%u capacity=%u\n",
                (unsigned)added, (unsigned)_numForeignMarketPlayers2,
                (unsigned)SWOS_AI_CAREER_POOL_CAPACITY);
    fprintf(stderr, "[SWOS AI P2.1] complete inserted=%u total=%u capacity=%u\n",
            (unsigned)added, (unsigned)_numForeignMarketPlayers2,
            (unsigned)SWOS_AI_CAREER_POOL_CAPACITY);
    fflush(stderr);
}


/* SWOS AI P3.2: manual world-wide AI exchange simulation.
 * No call from new-career or season startup. The Transfers menu is the only trigger.
 * Original TEAM data are read only. Career overrides are replayed before matching.
 */
#define SWOS_AI_P3_MAX_CLUBS 2048
#define SWOS_AI_P3_MAX_EXCHANGES 12000

typedef struct SWOS_AI_P3Club {
    uint16_t id;
    uint8_t country;
    uint8_t league;
    char name[18];
    PlayerFile players[16];
    uint8_t usedSlots[16];
    uint8_t requested;
    uint8_t completed;
} SWOS_AI_P3Club;

static int SWOS_AI_P3Cmp(const void *aa, const void *bb)
{
    const SWOS_AI_P3Club *a = (const SWOS_AI_P3Club *)aa;
    const SWOS_AI_P3Club *b = (const SWOS_AI_P3Club *)bb;
    if (a->country != b->country) return (int)a->country - (int)b->country;
    if (a->league != b->league) return (int)a->league - (int)b->league;
    return strcmp(a->name, b->name);
}

/* Remember players moved in the preceding two button clicks.  This is a
 * session-local safeguard, not persisted in save games.  The player's full
 * record is fingerprinted, so moving clubs does not change their identity. */
#define SWOS_AI_P3_RECENT_SIZE 65536u
static uint64_t SWOS_AI_P3RecentKeys[SWOS_AI_P3_RECENT_SIZE];
static uint32_t SWOS_AI_P3RecentClicks[SWOS_AI_P3_RECENT_SIZE];
static uint32_t SWOS_AI_P3ClickNumber;

/* JSON career save hooks. Older saves without these keys reset only the
 * cooldown, never the actual transfer records. */
void SWOS_AI_P3ResetHistory(void)
{
    memset(SWOS_AI_P3RecentKeys, 0, sizeof(SWOS_AI_P3RecentKeys));
    memset(SWOS_AI_P3RecentClicks, 0, sizeof(SWOS_AI_P3RecentClicks));
    SWOS_AI_P3ClickNumber = 0;
}

uint32_t SWOS_AI_P3GetClickNumber(void) { return SWOS_AI_P3ClickNumber; }

unsigned SWOS_AI_P3GetRecent(unsigned start, uint32_t *hi, uint32_t *lo,
                              uint32_t *last)
{
    for (unsigned i = start; i < SWOS_AI_P3_RECENT_SIZE; ++i) {
        uint32_t n = SWOS_AI_P3RecentClicks[i];
        if (!n || (uint32_t)(SWOS_AI_P3ClickNumber - n) > 2u) continue;
        uint64_t key = SWOS_AI_P3RecentKeys[i];
        if (!key) continue;
        *hi = (uint32_t)(key >> 32); *lo = (uint32_t)key;
        *last = n;
        return i + 1u;
    }
    return 0;
}

void SWOS_AI_P3LoadRecent(uint32_t hi, uint32_t lo, uint32_t last)
{
    uint64_t key = ((uint64_t)hi << 32) | lo;
    if (!key || !last || last > SWOS_AI_P3ClickNumber ||
        SWOS_AI_P3ClickNumber - last > 2u) return;
    unsigned first = (unsigned)((key ^ (key >> 32)) & (SWOS_AI_P3_RECENT_SIZE - 1));
    for (unsigned probe = 0; probe < SWOS_AI_P3_RECENT_SIZE; ++probe) {
        unsigned i = (first + probe) & (SWOS_AI_P3_RECENT_SIZE - 1);
        if (!SWOS_AI_P3RecentClicks[i] || SWOS_AI_P3RecentKeys[i] == key) {
            SWOS_AI_P3RecentKeys[i] = key;
            SWOS_AI_P3RecentClicks[i] = last;
            return;
        }
    }
}

void SWOS_AI_P3SetClickNumber(uint32_t n) { SWOS_AI_P3ClickNumber = n; }


static uint64_t SWOS_AI_P3Fingerprint(PlayerFile p)
{
    const unsigned char *bytes = (const unsigned char *)&p;
    uint64_t h = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(p); i++) {
        h ^= bytes[i];
        h *= UINT64_C(1099511628211);
    }
    return h ? h : 1;
}

static unsigned SWOS_AI_P3RecentIndex(uint64_t key, uint32_t click)
{
    unsigned first = (unsigned)((key ^ (key >> 32)) & (SWOS_AI_P3_RECENT_SIZE - 1));
    for (unsigned probe = 0; probe < SWOS_AI_P3_RECENT_SIZE; probe++) {
        unsigned i = (first + probe) & (SWOS_AI_P3_RECENT_SIZE - 1);
        uint32_t last = SWOS_AI_P3RecentClicks[i];
        if (SWOS_AI_P3RecentKeys[i] == key || !last ||
            (uint32_t)(click - last) > 2u)
            return i;
    }
    return first;
}

static int SWOS_AI_P3RecentlyMoved(PlayerFile p, uint32_t click)
{
    uint64_t key = SWOS_AI_P3Fingerprint(p);
    unsigned i = SWOS_AI_P3RecentIndex(key, click);
    uint32_t last = SWOS_AI_P3RecentClicks[i];
    return SWOS_AI_P3RecentKeys[i] == key && last &&
           (uint32_t)(click - last) <= 2u;
}

static void SWOS_AI_P3MarkMoved(PlayerFile p, uint32_t click)
{
    uint64_t key = SWOS_AI_P3Fingerprint(p);
    unsigned i = SWOS_AI_P3RecentIndex(key, click);
    SWOS_AI_P3RecentKeys[i] = key;
    SWOS_AI_P3RecentClicks[i] = click;
}

static uint32_t SWOS_AI_P3Seed = 0x534F5753u;
static uint32_t SWOS_AI_P3Random(void)
{
    SWOS_AI_P3Seed = SWOS_AI_P3Seed * 1664525u + 1013904223u;
    return SWOS_AI_P3Seed;
}

/* Use SWOS's own two-step price lookup, in game price units, rather than
 * comparing the raw price indices as if they were linear currency values. */
static unsigned SWOS_AI_P3Value(PlayerFile p)
{
    extern int8_t _avgTeamValues[];
    extern uint16_t _avgTeamValueBases[];
    int price = p.price;
    if (price < 0 || price > 49 || !p.playerName[0]) return 0;
    unsigned index = (uint8_t)_avgTeamValues[price];
    if (!index || index >= 92) return 0;
    return _avgTeamValueBases[index];
}

static unsigned SWOS_AI_P3Position(PlayerFile p)
{
    return (p.positionAndFace >> 5) & 7u;
}

static int SWOS_AI_P3IsValid(PlayerFile p)
{
    unsigned pos = SWOS_AI_P3Position(p);
    return pos <= 7 && SWOS_AI_P3Value(p) > 0;
}

/* Every 42-byte replacement identifies the target club and squad slot.
 * Updating its previous record avoids infinite growth over later seasons. */
static unsigned SWOS_AI_P3OverrideIndex(uint16_t teamId, unsigned slot)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    for (unsigned k = 0; k < _numForeignMarketPlayers2; k++) {
        const MarketPlayer *p = &_careerForeignMarketPlayers[k];
        const uint8_t *bytes = (const uint8_t *)&p->teamId;
        if (bytes[0] == (uint8_t)(teamId >> 8) &&
            bytes[1] == (uint8_t)teamId && (uint8_t)p->playerOrdinal == slot)
            return k;
    }
    return _numForeignMarketPlayers2;
}

static void SWOS_AI_P3WriteOverrideAt(uint16_t teamId, unsigned slot,
                                      PlayerFile incoming, unsigned index)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _lastLoadedExtNumber;
    extern uint8_t _viewRandomPhase;
    MarketPlayer *p = &_careerForeignMarketPlayers[index];
    ((uint8_t *)&p->teamId)[0] = (uint8_t)(teamId >> 8);
    ((uint8_t *)&p->teamId)[1] = (uint8_t)teamId;
    p->playerOrdinal = (int8_t)slot;
    p->padding = (int8_t)_viewRandomPhase;
    p->player = incoming;
    if (index == _numForeignMarketPlayers2) {
        ++_numForeignMarketPlayers2;
        ++_pendingForeignPlayerCount;
    }
    _lastLoadedExtNumber = 255;
}

/* Replacements alone do not change clubs already resident in the career's
 * selected-team and TMD caches. GetTeamPointer() consults both caches before
 * loading TEAM.NNN, so apply the new value to those live copies immediately.
 * These copies are also saved as part of the career. */
static unsigned SWOS_AI_P3SyncLiveSlot(uint16_t id, unsigned slot, PlayerFile p)
{
    extern uint16_t _numSelectedTeams;
    extern uint16_t _careerFileBuffer;
    extern uint8_t _tmdFileBuffer[];
    extern uint16_t _currentTeamNumber;
    extern uint8_t _currentTeam[];
    unsigned changed = 0;
    if (slot >= 16 || id == _selTeamNumber) return 0;
    unsigned n = _numSelectedTeams;
    if (n > 100) n = 100;
    for (unsigned i = 0; i < n; ++i) {
        TeamFileEx *t = &_selectedTeams[i];
        if (PACK_BE_U16(t->team.countryNumber, t->team.teamOrdinal) == id) {
            t->players[slot] = p;
            ++changed;
        }
    }
    n = _careerFileBuffer;
    if (n > 128) n = 128;
    TeamFileEx *tmd = (TeamFileEx *)_tmdFileBuffer;
    for (unsigned i = 0; i < n; ++i) {
        TeamFileEx *t = &tmd[i];
        if (PACK_BE_U16(t->team.countryNumber, t->team.teamOrdinal) == id) {
            t->players[slot] = p;
            ++changed;
        }
    }
    if (_currentTeamNumber == id) {
        TeamFileEx *t = (TeamFileEx *)_currentTeam;
        if (PACK_BE_U16(t->team.countryNumber, t->team.teamOrdinal) == id) {
            t->players[slot] = p;
            ++changed;
        }
    }
    return changed;
}

/* Reserves both override slots before doing anything so a full pool cannot
 * produce a one-sided exchange. */
static int SWOS_AI_P3Swap(SWOS_AI_P3Club *a, unsigned sa,
                          SWOS_AI_P3Club *b, unsigned sb)
{
    unsigned ia = SWOS_AI_P3OverrideIndex(a->id, sa);
    unsigned ib = SWOS_AI_P3OverrideIndex(b->id, sb);
    unsigned needed = (ia == _numForeignMarketPlayers2) +
                      (ib == _numForeignMarketPlayers2);
    if (_numForeignMarketPlayers2 + needed > SWOS_AI_CAREER_POOL_CAPACITY)
        return 0;
    PlayerFile pa = a->players[sa];
    PlayerFile pb = b->players[sb];
    /* After appending first entry the next new slot is one index later. */
    if (ia == _numForeignMarketPlayers2 && ib == _numForeignMarketPlayers2)
        ++ib;
    SWOS_AI_P3WriteOverrideAt(a->id, sa, pb, ia);
    SWOS_AI_P3WriteOverrideAt(b->id, sb, pa, ib);
    a->players[sa] = pb;
    b->players[sb] = pa;
    unsigned syncedA = SWOS_AI_P3SyncLiveSlot(a->id, sa, pb);
    unsigned syncedB = SWOS_AI_P3SyncLiveSlot(b->id, sb, pa);
    SWOS_AI_LOG("[SWOS AI CACHE] sync %04X:%u cached=%u; %04X:%u cached=%u\n",
                a->id, sa, syncedA, b->id, sb, syncedB);
    a->usedSlots[sa] = b->usedSlots[sb] = 1;
    return 1;
}

static unsigned SWOS_AI_P3AuditSlot(uint16_t id, unsigned slot,
                                    const PlayerFile *expected,
                                    unsigned *cacheChecked)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _numSelectedTeams, _careerFileBuffer, _currentTeamNumber;
    extern uint8_t _tmdFileBuffer[], _currentTeam[];
    unsigned fails = 0;
    unsigned idx = SWOS_AI_P3OverrideIndex(id, slot);
    if (idx >= _numForeignMarketPlayers2 ||
        memcmp(&_careerForeignMarketPlayers[idx].player, expected, sizeof(*expected))) {
        SWOS_AI_LOG("[SWOS AI AUDIT] FAIL override club=%04X slot=%u\n", id, slot);
        ++fails;
    }
    unsigned n = _numSelectedTeams < 100 ? _numSelectedTeams : 100;
    for (unsigned i=0;i<n;i++) {
        TeamFileEx *t = &_selectedTeams[i];
        if (PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal) != id) continue;
        ++*cacheChecked;
        if (memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
    }
    n = _careerFileBuffer < 128 ? _careerFileBuffer : 128;
    TeamFileEx *tmd=(TeamFileEx *)_tmdFileBuffer;
    for(unsigned i=0;i<n;i++) {
        TeamFileEx *t=&tmd[i];
        if(PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal)!=id) continue;
        ++*cacheChecked;
        if(memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
    }
    if(_currentTeamNumber==id) {
        TeamFileEx *t=(TeamFileEx *)_currentTeam;
        if(PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal)==id) {
            ++*cacheChecked;
            if(memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
        }
    }
    if(fails) SWOS_AI_LOG("[SWOS AI AUDIT] FAIL club=%04X slot=%u problems=%u\n",id,slot,fails);
    return fails;
}

static int SWOS_AI_P3MissingCountry(unsigned c)
{
    /* TEAM.072 contains custom/joke preset teams, not career clubs.
     * TEAM.080--085 are national-team continents and are excluded by
     * the collector's country <= 79 loop bound. Keep both outside AI swaps. */
    return c == 9 || (c >= 52 && c <= 54) || c == 56 ||
           c == 58 || c == 61 || c == 63 || c == 74 ||
           c == 57 || c == 59 || c == 70 || c == 72;
}

/* Load snapshots without LoadTeamFile(): the game's asset loader can retry
 * forever when a country is absent/unavailable, and changes global state.
 * Respect SWOS_DATA_JSON by trying the same JSON data loader first. The
 * fallback directly reads verified, raw TEAM.NNN bytes from disk. */
static int SWOS_AI_P3ReadCountry(unsigned country, uint8_t *buf, size_t cap,
                                  uint16_t *outCount)
{
    char filename[80];
    FILE *f = NULL;
    size_t size = 0;
    if (!buf || cap < 2 || !outCount) return 0;
    *outCount = 0;
    const char *json = getenv("SWOS_DATA_JSON");
    if (!(json && strcmp(json, "0") == 0)) {
        extern int DataJson_LoadTeamFileForCountry(int, uint8_t *, int);
        int n = DataJson_LoadTeamFileForCountry((int)country, buf, 93);
        if (n > 0 && n <= 93) {
            *outCount = (uint16_t)n;
            return 1;
        }
    }
    /* Resolve the same asset directory and case as OpenSWOS's loader.
     * The working directory is not necessarily the database directory. */
    char logical[40];
    char resolved[1024];
    snprintf(logical, sizeof logical, "data/team.%03u", country);
    const char *actual = swos_data_path(logical, resolved, sizeof resolved);
    f = fopen(actual, "rb");
    if (!f) {
        SWOS_AI_LOG("[SWOS AI WORLD D4] SKIP country=%u: cannot open %s\n",
                    country, actual);
        return 0;
    }
    SWOS_AI_LOG("[SWOS AI WORLD D4] OPEN country=%u path=%s\n", country, actual);
    /* TEAM.NNN files may be raw (2+684*N) or RNC-compressed. Read into a
     * separate bounded input buffer and decode to the validated snapshot.
     * In particular, never put compressed bytes in the output team buffer. */
    const size_t packedCap = 256u * 1024u;
    uint8_t *packed = (uint8_t *)malloc(packedCap + 32u);
    if (!packed) { fclose(f); return 0; }
    size = fread(packed, 1, packedCap + 1u, f);
    int io_error = ferror(f);
    fclose(f);
    if (io_error || size == 0 || size > packedCap) {
        SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: oversized/unreadable input size=%u\n",
                    country, (unsigned)size);
        free(packed);
        return 0;
    }
    if (size >= 18 && packed[0] == 'R' && packed[1] == 'N' && packed[2] == 'C') {
        unsigned method = packed[3];
        uint32_t unpacked = BE_READ_U32(packed + 4);
        uint32_t compressed = BE_READ_U32(packed + 8);
        if ((method != 1 && method != 2) || unpacked < 2 ||
            unpacked > cap || compressed > size - 18u || compressed == 0 ||
            (unpacked - 2u) % sizeof(TeamFileEx) != 0 ||
            (unpacked - 2u) / sizeof(TeamFileEx) > 93u) {
            SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: invalid RNC header method=%u unpacked=%u packed=%u size=%u\n",
                        country, method, unpacked, compressed, (unsigned)size);
            free(packed);
            return 0;
        }
        /* Decoder may read a few bytes past the last compressed word. */
        memset(packed + size, 0, 32);
        if (method == 1) RncDecompressV1(packed, buf);
        else             RncDecompressV2(packed, buf);
        size = unpacked;
        SWOS_AI_LOG("[SWOS AI WORLD D5] DECOMPRESSED country=%u method=%u bytes=%u\n",
                    country, method, (unsigned)size);
    } else {
        if (size > cap) {
            SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: raw data too large size=%u\n",
                        country, (unsigned)size);
            free(packed);
            return 0;
        }
        memcpy(buf, packed, size);
        SWOS_AI_LOG("[SWOS AI WORLD D5] RAW country=%u bytes=%u\n",
                    country, (unsigned)size);
    }
    free(packed);
    if (size < 2) return 0;
    unsigned count = ((unsigned)buf[0] << 8) | buf[1];
    if (!count || count > 93 || size != 2u + count * sizeof(TeamFileEx)) {
        SWOS_AI_LOG("[SWOS AI WORLD D3] SKIP country=%u: count=%u size=%u expected=%u\n",
                    country, count, (unsigned)size,
                    (unsigned)(2u + count * sizeof(TeamFileEx)));
        return 0;
    }
    /* Raw TEAM.NNN records use BE for the count and global team number.
     * JSON gives host-native records; only binary records reach this point. */
    TeamFileEx *rec = (TeamFileEx *)(buf + 2);
    for (unsigned i = 0; i < count; ++i)
        rec[i].team.globalTeamNumber =
            (int16_t)BE_READ_U16(&rec[i].team.globalTeamNumber);
    *(uint16_t *)buf = (uint16_t)count;
    *outCount = (uint16_t)count;
    return 1;
}

static unsigned SWOS_AI_P3Collect(SWOS_AI_P3Club *clubs)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    const size_t bufferSize = 2u + 93u * sizeof(TeamFileEx);
    uint8_t *snapshot = malloc(bufferSize);
    if (!snapshot) {
        SWOS_AI_LOG("[SWOS AI WORLD D3] ERROR: snapshot allocation failed\n");
        return 0;
    }
    unsigned clubCount = 0;
    for (unsigned country = 0; country <= 79 && clubCount < SWOS_AI_P3_MAX_CLUBS; country++) {
        if (SWOS_AI_P3MissingCountry(country)) {
            SWOS_AI_LOG("[SWOS AI WORLD D3] SKIP excluded country=%u\n", country);
            continue;
        }
        const char *skip = getenv("SWOS_AI_WORLD_SKIP_COUNTRY");
        if (skip && *skip) {
            char *endptr = NULL;
            long skipId = strtol(skip, &endptr, 10);
            if (endptr != skip && *endptr == '\0' && skipId == (long)country) {
                SWOS_AI_LOG("[SWOS AI WORLD D3] skip requested country=%u\n", country);
                continue;
            }
        }
        uint16_t count = 0;
        SWOS_AI_LOG("[SWOS AI WORLD D3] BEFORE read country=%u\n", country);
        if (!SWOS_AI_P3ReadCountry(country, snapshot, bufferSize, &count)) continue;
        SWOS_AI_LOG("[SWOS AI WORLD D3] AFTER read country=%u count=%u\n", country, count);
        const TeamFileEx *teams = (const TeamFileEx *)(snapshot + 2);
        for (unsigned i = 0; i < count && clubCount < SWOS_AI_P3_MAX_CLUBS; i++) {
            const TeamFileEx *team = &teams[i];
            if (team->team.countryNumber != country) continue;
            uint16_t id = (uint16_t)((country << 8) | team->team.teamOrdinal);
            if (id == _selTeamNumber) continue;
            SWOS_AI_P3Club *dst = &clubs[clubCount++];
            dst->id = id;
            dst->country = (uint8_t)country;
            dst->league = team->team.league;
            memcpy(dst->name, team->team.teamName, 17);
            dst->name[17] = 0;
            memcpy(dst->players, team->players, sizeof(dst->players));
            for (unsigned k = 0; k < _numForeignMarketPlayers2; k++) {
                const MarketPlayer *p = &_careerForeignMarketPlayers[k];
                const uint8_t *bytes = (const uint8_t *)&p->teamId;
                if (bytes[0] == (uint8_t)(id >> 8) &&
                    bytes[1] == (uint8_t)id &&
                    p->playerOrdinal >= 0 && p->playerOrdinal < 16)
                    dst->players[(unsigned)p->playerOrdinal] = p->player;
            }
        }
        SWOS_AI_LOG("[SWOS AI WORLD D3] FINISHED country=%u accumulated-clubs=%u\n", country, clubCount);
    }
    free(snapshot);
    SWOS_AI_LOG("[SWOS AI WORLD D3] BEFORE sorting clubs=%u\n", clubCount);
    qsort(clubs, clubCount, sizeof(*clubs), SWOS_AI_P3Cmp);
    SWOS_AI_LOG("[SWOS AI WORLD D3] AFTER sorting clubs=%u\n", clubCount);
    return clubCount;
}

/* An exact position and currency-value match must always hold. Requested
 * direction applies to the initiating club; the counterparty necessarily
 * receives the opposite value effect in a two-player swap. */
static int SWOS_AI_P3Candidate(PlayerFile outgoing, PlayerFile incoming,
                               int wantHigher)
{
    if (!SWOS_AI_P3IsValid(outgoing) || !SWOS_AI_P3IsValid(incoming)) return 0;
    if (SWOS_AI_P3Position(outgoing) != SWOS_AI_P3Position(incoming)) return 0;
    unsigned a = SWOS_AI_P3Value(outgoing), b = SWOS_AI_P3Value(incoming);
    if (b * 100u < a * 70u || b * 100u > a * 130u) return 0;
    return wantHigher ? b > a : b < a;
}

static void SWOS_AI_RealTransferTest(void)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    const uint32_t run = ++SWOS_AI_P3ClickNumber;
    SWOS_AI_LOG("[SWOS AI WORLD] CLICK=%u starting current pool=%u capacity=%u\n",
                run, (unsigned)_numForeignMarketPlayers2,
                (unsigned)SWOS_AI_CAREER_POOL_CAPACITY);
    SWOS_AI_P3Club *clubs = calloc(SWOS_AI_P3_MAX_CLUBS, sizeof(*clubs));
    if (!clubs) {
        SWOS_AI_LOG("[SWOS AI WORLD] ERROR: club snapshot allocation failed\n");
        return;
    }
    /* Each button click normally gets a fresh seed.  SWOS_AI_SEED provides
     * an explicit reproducible seed for debugging the SAME starting career. */
    const char *seedText = getenv("SWOS_AI_SEED");
    char *seedEnd = NULL;
    unsigned long fixedSeed = seedText ? strtoul(seedText, &seedEnd, 0) : 0;
    if (seedText && *seedText && seedEnd != seedText && !*seedEnd)
        SWOS_AI_P3Seed = (uint32_t)fixedSeed;
    else
        SWOS_AI_P3Seed = (uint32_t)time(NULL) ^
            (uint32_t)clock() * 2654435761u ^
            (uint32_t)_numForeignMarketPlayers2 * 2246822519u ^
            run * 3266489917u;
    if (!SWOS_AI_P3Seed) SWOS_AI_P3Seed = 0x534F5753u;
    SWOS_AI_LOG("[SWOS AI WORLD] CLICK=%u SEED=%u cooldown=2 clicks\n",
                (unsigned)run, (unsigned)SWOS_AI_P3Seed);
    unsigned clubCount = SWOS_AI_P3Collect(clubs);
    /* Optional test-only limit; without the variable the full world runs. */
    const char *clubLimit = getenv("SWOS_AI_WORLD_MAX_CLUBS");
    if (clubLimit && *clubLimit) {
        char *end = NULL;
        long n = strtol(clubLimit, &end, 10);
        if (end != clubLimit && !*end && n >= 2 && (unsigned long)n < clubCount)
            clubCount = (unsigned)n;
    }
    unsigned completed = 0, domestic = 0, international = 0;
    unsigned higher = 0, lower = 0, shortfall = 0;
    SWOS_AI_LOG("[SWOS AI WORLD] eligible AI clubs=%u\n", clubCount);
    if (clubCount < 2) { free(clubs); return; }
    unsigned countryFirst[256], countryLast[256];
    for (unsigned c = 0; c < 256; c++)
        countryFirst[c] = countryLast[c] = 0;
    for (unsigned i = 0; i < clubCount; i++) {
        unsigned c = clubs[i].country;
        if (countryFirst[c] == countryLast[c]) countryFirst[c] = i;
        countryLast[c] = i + 1;
        clubs[i].requested = (uint8_t)(4u + SWOS_AI_P3Random() % 3u);
    }

    /* Shuffle the order of initiating clubs each click.  The inventory stays
     * sorted so countryFirst/countryLast still describe valid partner ranges. */
    unsigned *order = malloc(clubCount * sizeof(*order));
    if (!order) { SWOS_AI_LOG("[SWOS AI WORLD] ERROR: order allocation failed\n"); free(clubs); return; }
    for (unsigned i = 0; i < clubCount; i++) order[i] = i;
    for (unsigned i = clubCount - 1; i > 0; i--) {
        unsigned j = SWOS_AI_P3Random() % (i + 1u);
        unsigned temp = order[i]; order[i] = order[j]; order[j] = temp;
    }
    unsigned cooldownRejected = 0;
    for (unsigned oi = 0; oi < clubCount; oi++) {
        unsigned i = order[oi];
        SWOS_AI_P3Club *a = &clubs[i];
        unsigned tries = 0;
        while (a->completed < a->requested && tries++ < 1100u) {
            int wantInternational = (SWOS_AI_P3Random() % 100u) < 15u;
            int wantHigher = (SWOS_AI_P3Random() % 100u) < 80u;
            unsigned sa = SWOS_AI_P3Random() % 16u;
            if (a->usedSlots[sa] || !SWOS_AI_P3IsValid(a->players[sa])) continue;
            if (SWOS_AI_P3RecentlyMoved(a->players[sa], run)) {
                ++cooldownRejected;
                continue;
            }
            /* Rotate possible partners randomly. All candidate clubs are real
             * loaded club records. No national squads, missing team IDs, or
             * human club can be selected as a partner. */
            unsigned j;
            if (wantInternational) {
                j = SWOS_AI_P3Random() % clubCount;
            } else {
                unsigned first = countryFirst[a->country];
                unsigned n = countryLast[a->country] - first;
                if (n < 2) continue;
                j = first + SWOS_AI_P3Random() % n;
            }
            if (j == i) continue;
            SWOS_AI_P3Club *b = &clubs[j];
            if (!!(b->country != a->country) != !!wantInternational ||
                b->completed >= b->requested) continue;
            unsigned startSlot = SWOS_AI_P3Random() % 16u;
            for (unsigned s = 0; s < 16; s++) {
                    unsigned sb = (startSlot + s) % 16u;
                    if (b->usedSlots[sb] ||
                        !SWOS_AI_P3Candidate(a->players[sa], b->players[sb], wantHigher))
                        continue;
                    if (SWOS_AI_P3RecentlyMoved(b->players[sb], run)) {
                        ++cooldownRejected;
                        continue;
                    }
                    PlayerFile outgoing = a->players[sa];
                    PlayerFile incoming = b->players[sb];
                    unsigned av = SWOS_AI_P3Value(outgoing);
                    unsigned bv = SWOS_AI_P3Value(incoming);
                    if (!SWOS_AI_P3Swap(a, sa, b, sb)) {
                        SWOS_AI_LOG("[SWOS AI WORLD] STOP: replacement pool capacity reached at %u\n",
                                    (unsigned)_numForeignMarketPlayers2);
                        goto finished;
                    }
                    char nameA[24], nameB[24];
                    memcpy(nameA, outgoing.playerName, 23); nameA[23] = 0;
                    memcpy(nameB, incoming.playerName, 23); nameB[23] = 0;
                    SWOS_AI_P3MarkMoved(outgoing, run);
                    SWOS_AI_P3MarkMoved(incoming, run);
                    ++a->completed;
                    ++b->completed;
                    ++completed;
                    if (wantInternational) ++international; else ++domestic;
                    if (wantHigher) ++higher; else ++lower;
                    SWOS_AI_LOG("[SWOS AI WORLD] EXCHANGE %u %s [%04X:%u] %s value=%u -> %s [%04X:%u]; %s value=%u -> %s; %s; %s\n",
                        completed, a->name, a->id, sa, nameA, av, b->name, b->id, sb,
                        nameB, bv, a->name, wantInternational ? "INTERNATIONAL" : "DOMESTIC",
                        wantHigher ? "HIGHER-INCOMING" : "LOWER-INCOMING");
                    break;
                }
        }
        if (a->completed < a->requested) {
            shortfall += a->requested - a->completed;
            SWOS_AI_LOG("[SWOS AI WORLD] SHORTFALL club=%s [%04X] requested=%u completed=%u\n",
                        a->name, a->id, a->requested, a->completed);
        }
    }
finished:
    unsigned auditSlots=0, auditCache=0, auditFailures=0;
    for(unsigned i=0;i<clubCount;i++) {
        for(unsigned s=0;s<16;s++) {
            if(!clubs[i].usedSlots[s]) continue;
            ++auditSlots;
            auditFailures += SWOS_AI_P3AuditSlot(clubs[i].id,s,
                                                 &clubs[i].players[s],&auditCache);
        }
    }
    SWOS_AI_LOG("[SWOS AI AUDIT] SUMMARY slots=%u cache-copies=%u failures=%u %s\n",
                auditSlots,auditCache,auditFailures,auditFailures?"FAIL":"PASS");
    SWOS_AI_LOG("[SWOS AI WORLD] SUMMARY clicks=%u exchanges=%u domestic=%u international=%u higher=%u lower=%u shortfall=%u cooldown-rejections=%u audit-failures=%u\n",
                run,completed,domestic,international,higher,lower,shortfall,cooldownRejected,auditFailures);
    SWOS_AI_LOG("[SWOS AI WORLD] COOLDOWN rejected-candidates=%u\n", cooldownRejected);
    SWOS_AI_LOG("[SWOS AI WORLD] DONE exchanges=%u domestic=%u international=%u higher=%u lower=%u unfilled-initiations=%u records=%u\n",
                completed, domestic, international, higher, lower, shortfall,
                (unsigned)_numForeignMarketPlayers2);
    free(order);
    free(clubs);
}

/* ================================================================
 * InitCareer  (SWOS2.s line 31722)
 *
 * Initializes a new career: clears season state, calls transfer market
 * init, calculates starting balance, copies competition data, sets
 * slot states, then calls InitializeNewSeason.
 * ================================================================ */
__attribute__((used, externally_visible))
void InitCareer(uint16_t teamNo)
{
    /* The cooldown belongs to this career session, not the previous one. */
    SWOS_AI_P3ResetHistory();
    _worldCupHostTeam = 20994;

    /* Clear 2000 bytes of diyLeagueSeasonStateTable (volatile to prevent memset optimization) */
    {
        volatile uint8_t *p = _diyLeagueSeasonStateTable;
        for (int16_t i = 1999; i >= 0; i--)
            *p++ = 0;
    }

    DIY_LoadLeagueScratchFromTable();

    _sameNationalCallFlag = 0;
    _careerSeasonsCompleted = 0;
    _boardConfidenceHistory0 = 0;
    _boardConfidenceHistory1 = 0;
    _boardConfidenceHistory2 = 0;
    _boardConfidenceHistory3 = 0;
    _boardConfidenceScore = 0;
    _gamesPlayed = 0;
    _numWins = 0;
    _numDrawn = 0;
    _numLost = 0;
    _goalsScored = 0;
    _goalsTaken = 0;
    _careerReputationPoints = 0;
    _winCupBonusCount = 0;
    _minorTrophyBonusCount = 0;
    _majorTrophyBonusCount = 0;
    _playPromotedAnimation = 0;
    _playRelegatedAnimation = 0;
    _teamsLoaded = 0;
    _poolplyrLoaded = 0;
    _careerFileBuffer = 0;
    _previousBoardConfidence = 0xFFFF;
    _playWinCupAnimation = 0;
    _minorTrophyBonusActive = 0;
    _leagueTitleBonusActive = 0;
    _majorTrophyBonusActive = 0;
    _cupBonus1Active = 0;
    _cupBonus2Active = 0;
    _wonWorldCup = 0;
    _wonContinentalCup = 0;
    _qualifyingStage1PendingFlag0 = 0;
    _qualifyingStage1PendingFlag1 = 0;
    _qualifyingStage2PendingFlag0 = 0;
    _qualifyingStage2PendingFlag1 = 0;
    _wonInvitationTournament = 0;

    Career_InitTransferMarketState();
    InitCareerVariables(4);

    uint32_t balance = CalculateStartingBalance(teamNo);
    Career_InitializeSelectedTeamAndStartingBalance(teamNo, (int32_t)balance);

    _careerFileBuffer = 0;

    LoadSomeEuroCup(_euroCup);
    LoadSomeEuroCup(_cupWinnersCup);
    LoadSomeEuroCup(_uefaCup);

    /* Copy careerContests table entries: {src, dest, count} until
     * src == 0xFFFFFFFF. Iterate as the typed CompetitionCareer[]
     * — on 64-bit each entry is 18 bytes (two 8-byte pointers + uint16);
     * the old byte-stride walk assumed 10-byte entries and would slip
     * 8 bytes between each iteration. Volatile cursors prevent LTO
     * from eliminating the ROM→BSS copies (same pattern as the tactic
     * copy bug). */
    extern CompetitionCareer _careerContests[];
    for (CompetitionCareer *e = _careerContests;
         (uintptr_t)e->competition != 0xFFFFFFFFu; e++) {
        volatile uint8_t *src = (volatile uint8_t *)e->competition;
        volatile uint8_t *dst = (volatile uint8_t *)e->competitionCopy;
        uint16_t count = e->templateSize;
        for (int16_t i = count - 1; i >= 0; i--)
            *dst++ = *src++;
    }

    _teamsLoaded = 0;
    _primaryCompetitionBuf.tourn.header.slotStateA = 3;
    _primaryCompetitionBuf.tourn.header.slotStateB = 2;
    _primaryCompetitionBuf.tourn.header.slotStateC = 8;
    _competitionContextBuf.tourn.header.slotStateA = 3;
    _competitionContextBuf.tourn.header.slotStateB = 2;
    _competitionContextBuf.tourn.header.slotStateC = 8;
    _tertiaryCompetitionContextBuf.tourn.header.slotStateA = 3;
    _tertiaryCompetitionContextBuf.tourn.header.slotStateB = 2;
    _tertiaryCompetitionContextBuf.tourn.header.slotStateC = 8;
    _euroCupsCompetitionContextBuf.tourn.header.slotStateA = 3;
    _euroCupsCompetitionContextBuf.tourn.header.slotStateB = 2;
    _euroCupsCompetitionContextBuf.tourn.header.slotStateC = 8;
    _currentSeasonShowing = 0xFFFF;
    _seasonPlaying--;
    InitializeNewSeason();
    /* AI transfers are strictly manual. No P1/P2 smoke or stress tests
     * may run during career creation, regardless of environment variables. */
    _careerLoadedFlag = 0;
}

/* ================================================================
 * ContinueCareerNextSeason  (SWOS2.s line 32355)
 *
 * If the manager has accepted a new job offer, show leaving message,
 * update competitions, init team with new funds, then init new season.
 * Otherwise, show chairman opinion, update competitions, recalculate
 * starting balance, then init new season.
 * ================================================================ */
__attribute__((used, externally_visible))
void ContinueCareerNextSeason(void)
{
    extern uint16_t _gameType;
    SWOS_AUTOSKIP_LOG("[bug1] ContinueCareerNextSeason ENTER: youreSacked=%u "
                      "teamNoOfferAccepted=0x%04X gameType=%u",
                      (unsigned)_youreSacked,
                      (unsigned)_teamNoOfferAccepted,
                      (unsigned)_gameType);
    if (_teamNoOfferAccepted != 0xFFFF) {
        if (_youreSacked == 0) {
            ShowLeavingMessage();
        }
        SWOS_AUTOSKIP_LOG("[bug1] ContinueCareerNextSeason: offer-accepted branch");
        Career_AdvanceToNextSeason_UpdateCompetitions();
        Career_ContinueToNextSeasonInitTeamAndFunds(_teamNoOfferAccepted, _teamFundsAccepted);
        InitializeNewSeason();
    } else {
        SWOS_AUTOSKIP_LOG("[bug1] ContinueCareerNextSeason: chairman branch (no offer accepted)");
        ShowChairmanOpinionAboutTheSeason();
        SWOS_AUTOSKIP_LOG("[bug1] back from ShowChairmanOpinion: youreSacked=%u", (unsigned)_youreSacked);
        Career_AdvanceToNextSeason_UpdateCompetitions();
        SWOS_AUTOSKIP_LOG("[bug1] back from AdvanceToNextSeason: youreSacked=%u", (unsigned)_youreSacked);
        InitializeStartingBalanceForNewSeason();
        SWOS_AUTOSKIP_LOG("[bug1] back from InitStartingBalance: youreSacked=%u", (unsigned)_youreSacked);
        InitializeNewSeason();
        SWOS_AUTOSKIP_LOG("[bug1] back from InitializeNewSeason: youreSacked=%u", (unsigned)_youreSacked);
    }
    /* Run one automatic worldwide AI transfer batch for each completed
     * season transition.  This path is not used by InitCareer(), so a new
     * career does not receive an additional transfer batch on creation.
     * Run after InitializeNewSeason() in both job-change and stay branches:
     * the new managed club is then established and excluded by the existing
     * AI transfer engine.  The manual Transfers-menu button remains active. */
    SWOS_AI_LOG("[SWOS AI AUTO] BEGIN season=%u records=%u\n",
                (unsigned)_careerSeasonsCompleted,
                (unsigned)_numForeignMarketPlayers2);
    SWOS_AI_RealTransferTest();
    SWOS_AI_LOG("[SWOS AI AUTO] END season=%u records=%u\n",
                (unsigned)_careerSeasonsCompleted,
                (unsigned)_numForeignMarketPlayers2);
    SWOS_AUTOSKIP_LOG("[bug1] ContinueCareerNextSeason EXIT");
}

/* ================================================================
 * Career_TeamInEuroCup
 *
 * Returns 1 if selTeamNumber is in cupCopy's static team list (the player's
 * club plays this euro cup), else 0. Shared by the season-init enrolment
 * (Career_SelectEuroCupSlotIfTeamParticipates) and the load-time slot-3
 * re-derivation (RebuildPointersAfterLoad) so both agree on participation —
 * see the load-fixup site for why a divergence panics 0x6006.
 * ================================================================ */
static int Career_TeamInEuroCup(uint8_t *cupCopy)
{
    /* team list = cupCopy + 7 + cupCopy[7], with the 64-bit text[2] widening
     * compensation (the static competition's unk1[] is packed BE (country,
     * ordinal) byte pairs; read via BE_READ_U16 to match selTeamNumber, which
     * is PACK_BE_U16(country, ordinal) everywhere). */
    uint8_t numEntries = cupCopy[7];
    uint16_t numEntriesAdj = (uint16_t)numEntries +
                             2u * (sizeof(char *) - sizeof(uint32_t));
    const uint8_t *teamList = (const uint8_t *)(cupCopy + 7 + numEntriesAdj);

    /* number of teams depends on competition type (byte at offset 1) */
    uint8_t compType = cupCopy[1];
    int16_t numTeams;
    if (compType == 1)      numTeams = (int8_t)cupCopy[0xA];
    else if (compType == 2) numTeams = (int8_t)cupCopy[0xF];
    else                    numTeams = (int8_t)cupCopy[0xD];

    uint16_t target = _selTeamNumber;
    for (int16_t i = (int16_t)(numTeams - 1); i >= 0; i--) {
        uint16_t entry = BE_READ_U16(teamList);
        teamList += 2;
        if (entry == target)
            return 1;   /* participating */
    }
    return 0;           /* not in this cup */
}

/* Career_TeamInLoadedEuroSlot — non-panicking membership probe for the LOADED
 * slot-3 buffer (a DIYCompetition), used by the load-time slot-3 re-derivation.
 *
 * It answers "is selTeamNumber genuinely a participant in this euro cup?" using
 * the SAME list `FindTeamInDIY` searches — the loaded buffer's team list
 * (teamIds for a type-2 tournament, cupTeamIds for a type-1 cup, each via the
 * buffer's own index table). This is the AUTHORITATIVE source for the loaded
 * season: the slot-3 buffer is saved (FA blob) so it holds the season's real
 * euro field, whereas `euroCupCopy` is reconstructed from its STATIC template on
 * load (save_format_json.c comp_copy_region) and holds DEFAULT teams — so
 * `Career_TeamInEuroCup(euroCupCopy)` gives a FALSE NEGATIVE for a club that's
 * in the saved season's cup but not the default template, leaving
 * quaternaryCompetitionPtr NULL and crashing the cached slot-3 game-list row in
 * `CareerGameListCommon` on `compInfo[5]`. Gating on the buffer keeps this
 * consistent with `FindTeamInDIY`: gate-pass => `FindTeamInDIY(team, buffer)`
 * also finds the team (no 0x6006); gate-fail => slot stays inactive (preserves
 * 1da09fa's protection — a true non-participant is in neither buffer nor
 * template). Mirrors `FindTeamInDIY`'s per-type search but returns found/
 * not-found instead of panicking. */
static int Career_TeamInLoadedEuroSlot(DIYCompetition *comp)
{
    uint16_t numTeams = (uint16_t)comp->numTeams;
    uint16_t target   = _selTeamNumber;

    if (comp->competitionType == 2) {
        /* DIY_TOURNAMENT (e.g. European Champions Cup) — teamIds[teamIndexOrder[i]]. */
        uint16_t *teamIds = ((DIYTournamentState *)comp)->teamIds;
        int8_t   *order   = (int8_t *)((DIYTournamentState *)comp)->teamIndexOrder;
        for (uint16_t i = 0; i < numTeams; i++) {
            int16_t idx = (int16_t)(int8_t)order[i];
            if (BE_READ_U16(&teamIds[idx]) == target)
                return 1;
        }
    } else if (comp->competitionType == 1) {
        /* DIY_CUP (Cup Winners'/UEFA) — cupTeamIds[teamGridPairings[i]]. */
        DIYCompetitionEx *ex      = (DIYCompetitionEx *)comp;
        uint16_t *cupTeamIds      = ex->cupTeamIds;
        int8_t   *pairings        = (int8_t *)ex->teamGridPairings;
        for (uint16_t i = 0; i < numTeams; i++) {
            int16_t idx = (int16_t)(int8_t)pairings[i];
            if (BE_READ_U16(&cupTeamIds[idx]) == target)
                return 1;
        }
    }
    return 0;           /* not in this loaded euro slot */
}

/* ================================================================
 * Career_SelectEuroCupSlotIfTeamParticipates  (SWOS2.s line 32087)
 *
 * Check if selTeamNumber is in the euro cup's team list.
 * If not found, return 1 (non-zero, "not participating").
 * If found, set up slot 3 (quaternary) for this euro cup and return 0.
 *
 * in:  a0 = euroCupCopy pointer, d7 = euroCupIndex (0/1/2)
 * out: d0 = 0 if participating, 1 if not
 * ================================================================ */
__attribute__((used, externally_visible))
uint16_t Career_SelectEuroCupSlotIfTeamParticipates(uint8_t *cupCopy, uint16_t euroCupIndex)
{
    /* Participation test (shared with the load-time slot-3 re-derivation in
     * RebuildPointersAfterLoad). */
    if (!Career_TeamInEuroCup(cupCopy))
        return 1;  /* not participating */
    {
#ifdef SWOS_DEBUG_AUTOSKIP
        SWOS_AUTOSKIP_LOG("[eurocup] team 0x%04X qualifies euroCupIndex=%u (cupCopy=%p) "
                          "-> activating slot 3",
                          (unsigned)_selTeamNumber, (unsigned)euroCupIndex, (void *)cupCopy);
#endif
        /* Save quaternary slot state */
        uint16_t savedA = _euroCupsCompetitionContextBuf.tourn.header.slotStateA;
        uint16_t savedB = _euroCupsCompetitionContextBuf.tourn.header.slotStateB;
        uint16_t savedC = _euroCupsCompetitionContextBuf.tourn.header.slotStateC;

        /* Set slot 3 for this euro cup (d0=3, d1=euroCupIndex, d2=0) */
        Career_SetCurrentCompetitionSlot(cupCopy, 3, 0, 0);

        /* quaternaryCompStringOffset == euroCupsCompetitionContext+0x28
         * namePtr on Amiga (slot 3). */
        ((SeasonInformations *)GetCurrentSeasonPointer())->strPtr3 =
            ((DIYCompetition *)_careerCompetitionSlotPtrTable[3])->namePtr;

        /* Restore quaternary slot state */
        _euroCupsCompetitionContextBuf.tourn.header.slotStateC = savedC;
        _euroCupsCompetitionContextBuf.tourn.header.slotStateB = savedB;
        _euroCupsCompetitionContextBuf.tourn.header.slotStateA = savedA;

        _euroCupSlotIndex = euroCupIndex;
        return 0;  /* participating */
    }
}

/* ================================================================
 * Career_SelectCopaLibertadoresSlotIfTeamParticipates
 *
 * CONMEBOL analog of Career_SelectEuroCupSlotIfTeamParticipates: if the player's
 * club is in this season's Copa Libertadores field (the 20 teams earned by the
 * 10 CONMEBOL leagues — already populated into Copa's *Copy seed list by the
 * qualifier in Career_AdvanceToNextSeason_UpdateCompetitions, which runs before
 * this), claim the quaternary slot (slot 3) for Copa so the player plays it
 * interactively (Career_UpdateCopaLibertadoresCompetitionSlot's
 * `== _quaternaryCompetitionPtr` branch).
 *
 * Tried only as the cascade fallback AFTER the three euro cups, so it can fire
 * only for a club in NONE of them — i.e. a South-American club. Copa reuses the
 * slot-3 context buffer (shared with the euro cups, mutually exclusive), so the
 * same slot-state bookkeeping applies. Reuses the static Career_TeamInEuroCup
 * field test (it is generic over a type-2 tournament *Copy). The Copa *Copy is
 * reached through Career_CopaWorkingCopy (src/career_copa.c).
 *
 * Copa claims slot 3 with _euroCupSlotIndex = 3 (the euro cups use 0/1/2). That
 * SAVED index drives BOTH (a) the load-time slot-3 re-derivation
 * (RebuildPointersAfterLoad below), so a saved CONMEBOL career re-loads with Copa
 * still interactive rather than auto-advancing, and (b) the continental-cup win
 * rewards in career_status.c, where index 3 is treated as euroCup-tier (major
 * trophy + euroCup financial bonus) — Copa Libertadores is South America's premier
 * club cup. The in-session play path branches on _quaternaryCompetitionPtr, not the
 * index.
 *
 * out: 0 if participating, 1 if not (or Copa not installed).
 * ================================================================ */
__attribute__((used, externally_visible))
uint16_t Career_SelectCopaLibertadoresSlotIfTeamParticipates(void)
{
    extern Competition *Career_CopaWorkingCopy(void);
    uint8_t *copaCopy = (uint8_t *)Career_CopaWorkingCopy();
    if (!copaCopy)
        return 1;  /* Copa not installed (e.g. SWOS_DATA_JSON disabled) */
    if (!Career_TeamInEuroCup(copaCopy))
        return 1;  /* player's club is not in the Copa field this season */

    /* Save quaternary slot state */
    uint16_t savedA = _euroCupsCompetitionContextBuf.tourn.header.slotStateA;
    uint16_t savedB = _euroCupsCompetitionContextBuf.tourn.header.slotStateB;
    uint16_t savedC = _euroCupsCompetitionContextBuf.tourn.header.slotStateC;

    /* Claim slot 3 for Copa */
    Career_SetCurrentCompetitionSlot(copaCopy, 3, 0, 0);

    ((SeasonInformations *)GetCurrentSeasonPointer())->strPtr3 =
        ((DIYCompetition *)_careerCompetitionSlotPtrTable[3])->namePtr;

    /* Restore quaternary slot state */
    _euroCupsCompetitionContextBuf.tourn.header.slotStateC = savedC;
    _euroCupsCompetitionContextBuf.tourn.header.slotStateB = savedB;
    _euroCupsCompetitionContextBuf.tourn.header.slotStateA = savedA;

    /* Copa sentinel for the slot-3 index: euro cups are 0/1/2, Copa is 3. Saved,
     * and consumed by the load gate (RebuildPointersAfterLoad) + the
     * continental-cup rewards (career_status.c). */
    _euroCupSlotIndex = 3;

    return 0;  /* participating */
}

/* ================================================================
 * DIY_SelectCompetitionSlot4FromCurrentDivision  (SWOS2.s line 30972)
 *
 * If the primary competition is a league (type 0), check if it has
 * a linked DIY competition and set up slot 4 for it.
 * ================================================================ */
/* England's leagues — standalone globals, consolidated from the old
 * EnglishLeaguesBlock (competition_data.c). CompetitionLeague / Leagues come from
 * game_types.h; competition_data.h is kept out of this TU, so extern them here. */
extern CompetitionLeague _englishLeague;
extern Leagues _englishLeagues1, _englishLeagues2, _englishLeagues3;

__attribute__((used, externally_visible))
void DIY_SelectCompetitionSlot4FromCurrentDivision(void)
{
    uint8_t *comp = (uint8_t *)_currentCompetitionPtr;
    if (((Competition *)comp)->type != 0)   /* type at offset 1; 0 == league */
        return;

    /* The original asm followed divisions[currentDivisionIndex].qualifierLinkOffset
     * (was unk1) as a SELF-RELATIVE byte offset (from that byte) to a {diyComp,
     * diyCompetitionTablePtr} pointer pair. That offset was baked for the Amiga
     * name[8] / 4-byte-pointer layout, where it pointed at the per-division
     * playoff link pair. The struct consolidation widened name[] to [10] and
     * 64-bit grew the pointers, so the byte walk now lands on name[] (e.g.
     * aDivisionTwo) -> garbage diyComp -> slot-4 type-check panic 0x4459.
     * Resolve via the typed fields instead — the SAME per-division playoff
     * links career_season.c uses. qualifierLinkOffset == 0 => no link (e.g.
     * Premier, div 0).
     *
     * Pairs verified against the Amiga 4-byte offsets + live pointers:
     *   div 1: _main.playoffLinkPtrs[0]/[1] = {&div1Playoffs, &_div1}
     *   div 2: _div1.playoffs/nextGroup     = {&div2Playoffs, &_div2}
     *   div 3: _div2.playoffs/nextGroup     = {&div3Playoffs, &_div3} */
    CompetitionLeague *league = (CompetitionLeague *)comp;
    if (league->divisions[(uint16_t)_currentDivisionIndex].qualifierLinkOffset == 0)
        return;
    if (comp_id(comp) != COMPID_englishLeague)   /* comp non-NULL: deref'd at type check above */
        return;  /* only the English leagues carry these per-division links */

    uint8_t *diyComp;
    void    *diyTable;
    switch ((uint16_t)_currentDivisionIndex) {
    case 1:
        diyComp  = (uint8_t *)_englishLeague.playoffLinkPtrs[0];
        diyTable = _englishLeague.playoffLinkPtrs[1];
        break;
    case 2:
        diyComp  = (uint8_t *)_englishLeagues1.playoffs;
        diyTable = _englishLeagues1.nextGroup;
        break;
    case 3:
        diyComp  = (uint8_t *)_englishLeagues2.playoffs;
        diyTable = _englishLeagues2.nextGroup;
        break;
    default:
        return;
    }

    _diyCompetitionTablePtr = (uint32_t *)diyTable;
    Career_SetCurrentCompetitionSlot(diyComp, 4, 0, 0);
}

/* ================================================================
 * InitializeNewSeason  (SWOS2.s line 31818)
 *
 * Large function that initializes a new career season: increments
 * season counter, clears board confidence, sets up competition
 * slots (primary/secondary/tertiary/quaternary/diy), determines
 * if national selector, assigns euro cups, builds team budget rank
 * table, copies manager name, fills market players, inits finances,
 * and shows chairman message.
 * ================================================================ */
__attribute__((used, externally_visible))
void InitializeNewSeason(void)
{
    SWOS_AUTOSKIP_LOG("[bug1] InitializeNewSeason ENTER: youreSacked=%u "
                      "teamNoOfferAccepted=0x%04X selTeamNumber=0x%04X",
                      (unsigned)_youreSacked,
                      (unsigned)_teamNoOfferAccepted,
                      (unsigned)_selTeamNumber);
    _teamsLoaded = 0;
    _currentSeasonShowing++;
    if (_currentSeasonShowing == _careerParams.num_seasons)
        _currentSeasonShowing = 0;

    _selTeamSelectedPlayerPosition = 0xFFFF;

    /* Clear 4 entries of careerUnavailablePlayerList */
    /* careerUnavailablePlayerList holds 4 × { uint16 teamNumber,
     * uint16 playerIndex } entries at byte stride 4. Mark the first 4
     * teamNumber slots as empty (0xFFFF). */
    {
        uint16_t *upl = (uint16_t *)_careerUnavailablePlayerList;
        upl[0] = 0xFFFF;
        upl[2] = 0xFFFF;
        upl[4] = 0xFFFF;
        upl[6] = 0xFFFF;
    }

    _retired = 0;
    _youreSacked = 0;
    _competitionSlotUpdatedFlag = 0;
    _stayingInOldClub++;
    _seasonPlaying++;
    if (_seasonPlaying == 100)
        _seasonPlaying = 0;

    DIY_SaveLeagueScratchToTable();

    _boardConfidenceMod0 = 0;
    _boardConfidenceMod1 = 0;
    _boardConfidenceMod2 = 0;
    _boardConfidenceMod3 = 0;
    _boardConfidenceValue0 = 0;
    _boardConfidenceValue1 = 0;
    _boardConfidenceMod4 = 0;
    _boardConfidenceMod5 = 0;
    _boardConfidenceMod6 = 0;
    _boardConfidenceMod7 = 0;
    _boardConfidenceMod8 = 0;
    _boardConfidenceMod9 = 0;

    /* Get current season info pointer and fill it */
    SeasonInformations *si = (SeasonInformations *)GetCurrentSeasonPointer();
    si->clubNo            = _selTeamNumber;
    si->leagueStrPtr      = 0;
    si->strPtr1           = 0;
    si->strPtr2           = 0;
    si->strPtr3           = 0;
    si->comp1Result       = 0;
    si->comp2Result       = 0;
    si->comp3Result       = 0;
    si->comp4Result       = 0;
    si->bestScorerName[0] = 0;
    si->managerStatus     = 0;

    /* Copy team name to season info CLUBNAME */
    StringCopy((uint8_t *)_careerTeam.team.teamName, (uint8_t *)si->clubName);

    /* Verify team loaded and get current division index */
    TeamFileEx *teamFile = VerifyTeamLoaded(_selTeamNumber);
    uint16_t globalTeamNo = teamFile->team.globalTeamNumber;
    int8_t diyLeagueState = (int8_t)_diyLeagueSeasonStateTable[globalTeamNo];
    int16_t divIdx = (int16_t)diyLeagueState + (int16_t)_careerTeamLeague;
    _currentDivisionIndex = (uint16_t)divIdx;
    teamFile->team.league = (uint8_t)divIdx;

    _contractNegotiationActive = 0;
    _removedPlayerStatAccum = 0;
    Career_InitSeasonTransferMarketState();

    _teamSelectionKeyCacheEnd = 0;
    _isCareer = 0;
    _careerMatchSimulatedFlag = 0;
    _numNewJobOffers = 0;
    _numJobOffers = 0;
    _careerTeamLostMatch = 0;
    _currentMessage = 0;

    /* Fill careerLastMatchIndexByCompetitionSlot with 0xFF (162 bytes, volatile to avoid memset) */
    {
        volatile uint8_t *dst = _careerLastMatchIndexByCompetitionSlot;
        for (int16_t i = 161; i >= 0; i--)
            *dst++ = 0xFF;
    }

    /* Initialize competition slot pointer table */
    _careerCompetitionSlotPtrTable[0] = (uint8_t *)&_primaryCompetitionBuf;
    _careerCompetitionSlotPtrTable[1] = (uint8_t *)&_competitionContextBuf;
    _careerCompetitionSlotPtrTable[2] = (uint8_t *)&_tertiaryCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[3] = (uint8_t *)&_euroCupsCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[4] = (uint8_t *)&_diyCompetitionContextBuf;

    /* Clear all competition pointers */
    _currentCompetitionPtr = 0;
    _secondaryCompetitionPtr = 0;
    _tertiaryCompetitionPtr = 0;
    _quaternaryCompetitionPtr = 0;
    _diyCompetitionPtr = 0;

    _numSelectedTeams = 1;
    _teamSelectionKeyCacheCount = 0;
    _teamSelectionKeyCacheIndex = 0;
    _teamSelectionKeyCacheOffset = 0;
    _careerSlotProcessingInProgress = 0;
    _isNationalSelector = 0;

    /* Determine if this is a national team (country code 80..85 = CN_EUROPE..CN_OCEANIA) */
    uint16_t countryCode = _selTeamNumber >> 8;
    /* Walker over a country's competition pointer-array. Entries are
     * full-width pointers — `Competition *` — interleaved with -2/-1
     * sentinels. Iterating as `void **` makes the stride scale with
     * pointer width (4 B on Amiga/32-bit, 8 B on 64-bit) and lets the
     * sentinel comparison use the full pointer width via `(uintptr_t)-1`. */
    void **compTablePtr;

    if (countryCode >= 80 && countryCode <= 85) {
        /* National team selector */
        _isNationalSelector = 1;

        /* Check same nationality call */
        if (_selTeamNumber == _lastNationalityCall) {
            _sameNationalCallFlag = 1;
        }

        /* Look up team flags from region table. Walk as `void **` so
         * each slot is native pointer width (was `uint32_t *` which
         * read half a pointer on 64-bit and warned at the
         * (uint8_t *)<u32> cast). */
        void **regionTable = _careerTeamFlagsByRegionTable;
        uint16_t regionIdx = (uint8_t)(_selTeamNumber >> 8);  /* move.b — high byte = country */
        int16_t regionOffset = (int16_t)(int8_t)regionIdx - 80;
        uint8_t *flagsArray = (uint8_t *)regionTable[regionOffset];
        _someTeamFlags = flagsArray[_currentSeasonShowing % 20];  /* flags = fixed 20-entry period-4 euro-cup rotation; wraps for 35-season careers */

#ifdef SWOS_INJECT_WC
        /* DEBUG (SWOS_INJECT_WC, opt-in via `make SWOS_INJECT_WC=1`): force a
         * World Cup year for this national-team career, bypassing the natural
         * 4-year cycle, so the national-coach WC path runs every season — used
         * to reproduce the season-transition 0x8888 backup-clobber
         * (project_eurocup_national_backup_clobber). 0x80 selects the WC-year
         * branch in Career_SetCupPickList_ForSelectedRegion (called below);
         * qualifyingStage1PendingFlag0 = 0 picks the *invited/finals* pick list
         * (the actual WC tournament) rather than WC-qualifier friendlies — 0x80
         * alone would leave the team in friendlies with no WC. Flip the flag to
         * 1 if you want the qualifier path instead. National teams only (here). */
        someTeamFlags = 0x80;
        qualifyingStage1PendingFlag0 = 0;
        fprintf(stderr, "[SWOS_INJECT_WC] forcing World Cup for national team "
                "0x%04X (season %u)\n",
                (unsigned)selTeamNumber, (unsigned)currentSeasonShowing);
#endif

        /* Get nationality and team file */
        uint32_t natResult = GetNationalityAndTeamFile(_selTeamNumber);
        _nationalTeamNationality = (uint8_t)(natResult >> 16);  /* d0 */
        _nationalTeamFileId = (uint8_t)(natResult & 0xFF);       /* d1 */

        /* Look up season cycle flags. The A/B tables are 5 entries — one per
         * 4-year cycle across the fixed 20-season national scheduling period.
         * The original asm indexed seasonCycleNationalFlags[(season>>2)*2] with
         * NO wrap because careers were capped at 20 seasons (cycle 0..4, in
         * bounds). num_seasons now runs to 35, so cycle = season>>2 can reach 8
         * and must wrap into the 5 entries — otherwise it reads
         * seasonCycleNationalFlags[5..8] out of bounds and feeds garbage
         * World-Cup / Euro-Championship qualifier ids into the group fill.
         * cycle is <= 8 (CAREER_MAX_SEASONS caps season at 34), so a single
         * conditional subtract does the wrap — no `% 5` divide (slow on 68k). */
        uint16_t cycle = (_currentSeasonShowing >> 2) % 5u; /* 70-season safe tournament rotation */
        _worldCupQualifyingTeam = _seasonCycleNationalFlagsA[cycle];
        _euroChampionshipQualTeam = _seasonCycleNationalFlagsB[cycle];

        /* Set cup pick list for selected region (param unused, function writes to global) */
        Career_SetCupPickList_ForSelectedRegion(0);

        compTablePtr = (void **)&_careerCupPickList;
    } else {
        /* Club team */
        _nationalTeamFileCode = (uint8_t)(_selTeamNumber >> 8);  /* move.b — high byte */

        uint16_t tableIdx = _selTeamNumber >> 8;
        compTablePtr = (void **)_competitionsTable[tableIdx];
    }

    Career_UpdateLastNationalityCall();

    /* Set up primary competition (slot 0) */
    {
        uint16_t savedA = _primaryCompetitionBuf.tourn.header.slotStateA;
        uint16_t savedB = _primaryCompetitionBuf.tourn.header.slotStateB;
        uint16_t savedC = _primaryCompetitionBuf.tourn.header.slotStateC;

        uint8_t *comp = (uint8_t *)*compTablePtr;
        compTablePtr++;
        Career_SetCurrentCompetitionSlot(comp, 0, _currentDivisionIndex, 0);

        /* SWOS2.s reads (primaryCompStringOffset), which on Amiga IS
         * competitionFileBuffer+0x28 = the comp namePtr that
         * Career_SetCurrentCompetitionSlot just wrote. Our standalone
         * primaryCompStringOffset global is never populated (only zeroed),
         * so read the slot-0 buffer's namePtr directly. */
        ((SeasonInformations *)GetCurrentSeasonPointer())->leagueStrPtr =
            ((DIYCompetition *)_careerCompetitionSlotPtrTable[0])->namePtr;

        _primaryCompetitionBuf.tourn.header.slotStateC = savedC;
        _primaryCompetitionBuf.tourn.header.slotStateB = savedB;
        _primaryCompetitionBuf.tourn.header.slotStateA = savedA;
    }

    /* Skip the per-primary divisionIndex slot (one pointer-sized entry,
     * usually -2 = "no extra division pointer"). */
    compTablePtr++;

    /* Set up secondary competition (slot 1) if present */
    if ((uintptr_t)*compTablePtr != (uintptr_t)-1) {
        uint16_t savedA = _competitionContextBuf.tourn.header.slotStateA;
        uint16_t savedB = _competitionContextBuf.tourn.header.slotStateB;
        uint16_t savedC = _competitionContextBuf.tourn.header.slotStateC;

        uint8_t *comp = (uint8_t *)*compTablePtr;
        compTablePtr++;
        Career_SetCurrentCompetitionSlot(comp, 1, 0, 0);

        /* secondaryCompStringOffset == competitionContextBuf+0x28 namePtr on
         * Amiga (slot 1); read the buffer namePtr directly. */
        ((SeasonInformations *)GetCurrentSeasonPointer())->strPtr1 =
            ((DIYCompetition *)_careerCompetitionSlotPtrTable[1])->namePtr;

        _competitionContextBuf.tourn.header.slotStateC = savedC;
        _competitionContextBuf.tourn.header.slotStateB = savedB;
        _competitionContextBuf.tourn.header.slotStateA = savedA;

        /* Set up tertiary competition (slot 2) if present */
        if ((uintptr_t)*compTablePtr != (uintptr_t)-1) {
            uint16_t savedA2 = _tertiaryCompetitionContextBuf.tourn.header.slotStateA;
            uint16_t savedB2 = _tertiaryCompetitionContextBuf.tourn.header.slotStateB;
            uint16_t savedC2 = _tertiaryCompetitionContextBuf.tourn.header.slotStateC;

            comp = (uint8_t *)*compTablePtr;
            Career_SetCurrentCompetitionSlot(comp, 2, 0, 0);

            /* tertiaryCompStringOffset == tertiaryCompetitionContextBuf+0x28
             * namePtr on Amiga (slot 2). */
            ((SeasonInformations *)GetCurrentSeasonPointer())->strPtr2 =
                ((DIYCompetition *)_careerCompetitionSlotPtrTable[2])->namePtr;

            _tertiaryCompetitionContextBuf.tourn.header.slotStateC = savedC2;
            _tertiaryCompetitionContextBuf.tourn.header.slotStateB = savedB2;
            _tertiaryCompetitionContextBuf.tourn.header.slotStateA = savedA2;
        }
    }

    /* Try euro cups for slot 3 (quaternary); if the club is in none of them, try
     * Copa Libertadores (a South-American club). The quaternary slot holds the
     * player's ONE continental cup — euro and Copa are mutually exclusive, so this
     * cascade naturally routes a CONMEBOL club to Copa and everyone else to a euro
     * cup (or leaves slot 3 inactive). */
    if (Career_SelectEuroCupSlotIfTeamParticipates(_euroCupCopy, 0) != 0) {
        if (Career_SelectEuroCupSlotIfTeamParticipates(_cupWinnersCupCopy, 1) != 0) {
            if (Career_SelectEuroCupSlotIfTeamParticipates(_uefaCupCopy, 2) != 0) {
                Career_SelectCopaLibertadoresSlotIfTeamParticipates();
            }
        }
    }

    /* Set up DIY competition for slot 4 */
    DIY_SelectCompetitionSlot4FromCurrentDivision();

    InitializeNewSeason_BuildTeamBudgetRankTable();

    /* Determine leagueFirstRoundNumber */
    {
        uint16_t firstRound = 1;
        uint8_t *primaryComp = (uint8_t *)_currentCompetitionPtr;
        if (primaryComp[1] == 0) {
            firstRound = (uint16_t)primaryComp[9];
        }
        _leagueFirstRoundNumber = firstRound;
    }

    _seasonFinished = 0;
    _careerFinished = 0;

    /* Re-derive careerJobOfferPhaseBias from the freshly-built key cache.
     *
     * In SWOS2.s this byte OVERLAPS teamSelectionKeyCache[1] (adjacent-globals
     * BSS: `teamSelectionKeyCache: ds.b $1` immediately followed by
     * `careerJobOfferPhaseBias: ds.b $1`), so the cache builder writing entry-0's
     * sortKey there implicitly set the bias to the EARLIEST fixture's week. The
     * finance/job-offer code relies on that: it is the season-start week base for
     * lastFinancialWeekIndex and the `lastKey - bias` season span in
     * wageScalingDivisor. The SDL port promoted the byte to a standalone global
     * (career_key_cache.c) but never re-derived it, leaving it 0 — so
     * lastFinancialWeekIndex started at 0 and the FIRST weekly deduction
     * front-loaded the entire pre-season gap (both PLAYER WAGES BILL and GENERAL
     * RUNNING COSTS ~inflated on the first match, self-cancelling by season end
     * because wageScalingDivisor = lastKey grows to match). Restore the overlap
     * value explicitly so bias == entry-0 sortKey on every season init. */
    _careerJobOfferPhaseBias = ((TeamSelectionKeyCacheEntry *)_teamSelectionKeyCache)->sortKey;

    _viewRandomPhase = _careerJobOfferPhaseBias;

    /* Walk teamSelectionKeyCache to compute wageScalingDivisor */
    {
        uint8_t *cachePtr = _teamSelectionKeyCache;
        uint8_t lastKey = 0;
        int16_t count = _teamSelectionKeyCacheCount - 1;
        for (int16_t i = count; i >= 0; i--) {
            lastKey = ((TeamSelectionKeyCacheEntry *)cachePtr)->sortKey;
            uint16_t stride = ((TeamSelectionKeyCacheEntry *)cachePtr)->entryStride;
            cachePtr += stride;
        }
        uint16_t d0 = (uint16_t)(lastKey - _careerJobOfferPhaseBias) & 0xFF;
        if (d0 == 0)
            d0 = 1;
        _wageScalingDivisor = d0;
    }

    _lastFinancialWeekIndex = _careerJobOfferPhaseBias;

    InitTeams();

    /* Copy manager name into the team's coach-name slot */
    teamFile = VerifyTeamLoaded(_selTeamNumber);
    {
        uint8_t *namePos = (uint8_t *)teamFile->team.coachName;
        StringCopy(_managerNameBuffer, namePos);
        /* Find end of copied name and add space + surname */
        while (*namePos != 0)
            namePos++;
        *namePos++ = 0x20;  /* space */
        StringCopy(_managerSurnameBuffer, namePos);
    }

    FillForeignMarketPlayers();
    FillCareerTeamPlayers();
    Career_InitializeNewSeasonFinances();
    InitializeCareerSeasonFinancials();

    /* Show appropriate chairman message */
    if (_stayingInOldClub != 0) {
        if (_currentSeasonShowing == _careerParams.num_seasons - 1) {
            ShowEnjoyLastSeasonMessage();
        } else {
            ShowWishYouSuccessMessage();
        }
    } else {
        if (_currentSeasonShowing == _careerParams.num_seasons - 1) {
            ShowLastSeasonMessage();
        } else {
            ShowGreetingMessage();
        }
    }

    /* Post-message: trial players and international squad setup */
    if (_stayingInOldClub == 0) {
        TrainingMatch_AddTrialPlayersForNewSeason();
        if (_isNationalSelector != 0) {
            Career_InitInternationalSquadAndLookupTablesForNewSeason((TeamFileEx *)0);
        }
    }

}

/* ================================================================
 * InitNewSeason  (SWOS2.s line 32148)
 *
 * Simplified new season init used when continuing with a new team
 * (after job change). Sets up competition slots from the competition
 * table, counts non-computer squads, inits teams.
 *
 * in:  a0 = source comp record, d1 = divisionIndex
 * ================================================================ */
__attribute__((used, externally_visible))
void InitNewSeason(uint8_t *src, uint16_t divisionIndex)
{
    _retired = 0;
    uint8_t compTableIdx = (uint8_t)((Competition *)src)->teamNumber;
    _currentDivisionIndex = divisionIndex;

    /* Initialize competition slot pointer table */
    _careerCompetitionSlotPtrTable[0] = (uint8_t *)&_primaryCompetitionBuf;
    _careerCompetitionSlotPtrTable[1] = (uint8_t *)&_competitionContextBuf;
    _careerCompetitionSlotPtrTable[2] = (uint8_t *)&_tertiaryCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[3] = (uint8_t *)&_euroCupsCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[4] = (uint8_t *)&_diyCompetitionContextBuf;

    _currentCompetitionPtr = 0;
    _secondaryCompetitionPtr = 0;
    _tertiaryCompetitionPtr = 0;
    _quaternaryCompetitionPtr = 0;
    _diyCompetitionPtr = 0;

    _teamSelectionKeyCacheCount = 0;
    _teamSelectionKeyCacheIndex = 0;
    _teamSelectionKeyCacheOffset = 0;
    _careerSlotProcessingInProgress = 0;

    /* Look up competitions table by index — entries are full-width
     * pointers (8 B on 64-bit, 4 B on Amiga). Iterate as `void **` so
     * stride/sentinel scale with pointer width — see equivalent walker
     * in `InitializeNewSeason`. */
    void **compTable = (void **)_competitionsTable[compTableIdx & 0xFF];

    /* Slot 0: primary competition */
    uint8_t *comp = (uint8_t *)*compTable;
    compTable++;
    Career_SetCurrentCompetitionSlot(comp, 0, _currentDivisionIndex, 1);

    /* Skip the per-primary divisionIndex slot (one pointer entry, -2 = "none"). */
    compTable++;

    /* Slot 1: secondary competition (if present) */
    if ((uintptr_t)*compTable != (uintptr_t)-1) {
        comp = (uint8_t *)*compTable;
        compTable++;
        Career_SetCurrentCompetitionSlot(comp, 1, 0, 0);

        /* Slot 2: tertiary competition (if present) */
        if ((uintptr_t)*compTable != (uintptr_t)-1) {
            comp = (uint8_t *)*compTable;
            Career_SetCurrentCompetitionSlot(comp, 2, 0, 0);
        }
    }

    _seasonFinished = 0;
    _careerFinished = 0;

    /* Count non-computer-controlled squads */
    _seasonNumberOfSquads = 0;
    uint16_t *squadTeams = _seasonSquadTeamNumbers;
    TeamFileEx *teamPtr = _selectedTeams;
    int16_t numTeams = _numSelectedTeams - 1;
    for (int16_t i = numTeams; i >= 0; i--) {
        if (teamPtr->team.teamControls != 1) {
            _seasonNumberOfSquads++;
            *squadTeams++ = PACK_BE_U16(teamPtr->team.countryNumber,
                                        teamPtr->team.teamOrdinal);
        }
        teamPtr++;
    }

    if (_seasonNumberOfSquads == 0)
        _careerFinished = 1;

    _viewRandomPhase = _careerJobOfferPhaseBias;
    InitTeams();
}

/* ================================================================
 * RebuildPointersAfterLoad  (no asm equivalent — new code)
 *
 * Several fields in the careerFileBuffer overlay region (see SAVE_DATA.md)
 * are typed as absolute pointers — valid when the save was written but
 * meaningless in any subsequent binary build (or any other machine). The
 * JSON save format never stores these as raw addresses; it serializes a
 * stable IDENTITY instead (competition ID, buffer index, string-pool key).
 * After `SaveJson_LoadCareer` restores the typed fields, this function
 * re-derives every pointer field from its identity (current run's buffer
 * address, competition ID, etc.).
 *
 * This is now the ONLY pointer fixup. The legacy Amiga-absolute-address
 * translation LUT (`_TranslateAmigaAddressesAfterLoad` /
 * `save_pointer_lut.c` / `generated/amiga_address_map.c`) was deleted once
 * the raw-dump readers were retired — JSON identities make it unnecessary.
 *
 * Fields handled (see SAVE_DATA.md "Pointer-field summary for .CAR"):
 *
 *   1. careerCompetitionSlotPtrTable[5]  — five competition slot bases.
 *      Re-derived from `&competitionFileBuffer / &competitionContextBuf /
 *      &tertiaryCompetitionContextBuf / &euroCupsCompetitionContext /
 *      &diyCompetitionContextBuf` in the current build's BSS (slots 0/3
 *      still SLOT_ALIAS into their bufs; slots 1, 2 & 4 are the typed buffers).
 *
 *   2. diyCompetitionStart — runtime DIY working buffer (separate 4 KB
 *      BSS, NOT inside the careerFileBuffer overlay). Repopulated
 *      from the loaded slot-0 (primary competition) state.
 *
 *   3. Embedded `nameSource` pointers inside each cup *Copy record at
 *      offset (5 + nameOffset). Copied from the matching static cup
 *      template via the careerContests table.
 *
 *   4. Top-level competition pointers
 *      `currentCompetitionPtr / secondaryCompetitionPtr /
 *      tertiaryCompetitionPtr / diyCompetitionPtr /
 *      diyCompetitionTablePtr`. Re-derived from competitionsTable +
 *      saved selTeamNumber (or careerCupPickList for national
 *      selectors), mirroring `InitializeNewSeason` lines 994-1067
 *      but without re-initialising slot state.
 *
 * Not yet handled (will be layered in as we hit menus that read
 * them):
 *
 *   *CompStringOffset      — would re-derive from string-table
 *                              lookup by competition.
 *   quaternaryCompetitionPtr (slot-3 euro cup) — needs a saved-state
 *                              probe to disambiguate which cup. NULLed
 *                              for now; the slot is reachable via
 *                              careerCompetitionSlotPtrTable[3].
 *   tmdFileBuffer/careerTeam runtime pointers
 *                          — should be zeroed; rebuilt at match start.
 *
 * On the Amiga C port (post back-port) this function is identical —
 * neither port pins data at fixed addresses anymore, so both need the
 * same rebuild.
 * ================================================================ */
extern void Career_LoadCompetitionSlotStateFromBuffer(uint8_t *src);
/* declared at top of file (line 615) — uint16_t *teamList */

/* Step 3 helper: walk careerContests and re-derive each *Copy
 * record's embedded text[] pointer pair.
 *
 * Each cup record carries a signed `nameOffset` byte at offset 5. At
 * `record + 5 + nameOffset` lives an inline `text[2]` array — two
 * 4-byte pointers to static name strings (e.g. text[0] = "EUROPEAN
 * CHAMPIONS", text[1] = "EUROPEAN CUP"). The remaining bytes after
 * those 8 are the `unk1[]` lookup table (scalars, not pointers).
 *
 * The static template carries the canonical pointer values for the
 * current build; the saved *Copy carries stale values from the
 * saving build. Copy both pointers from src→dest. */
static void Rebuild_CupRecordNameSourcePointers(void)
{
    /* Iterate as typed CompetitionCareer[] — entry stride is 10 on
     * 32-bit, 18 on 64-bit. The text[] copy reads/writes two pointer-
     * width entries (4 B each → 8 B on 32-bit, 8 B each → 16 B on
     * 64-bit), so use sizeof(void *) and memcpy 2*sizeof(void *). */
    extern CompetitionCareer _careerContests[];
    for (CompetitionCareer *e = _careerContests;
         (uintptr_t)e->competition != 0xFFFFFFFFu; e++) {
        uint8_t *src  = (uint8_t *)e->competition;
        uint8_t *dest = (uint8_t *)e->competitionCopy;
        /* Copy the 2 name pointers (full + abbreviation) into the comp copy.
           dest is a byte copy of src, so its name table sits at the same place. */
        char **srcName = comp_name_table((Competition *)src);
        if (srcName) {
            memcpy((char **)comp_name_table((Competition *)dest),
                   srcName, 2 * sizeof(void *));
        }
    }
}

/* Step 4 helper: re-derive the top-level competition slot pointers.
 *
 * Mirrors the lookup in `InitializeNewSeason` (lines 994-1067) but
 * does NOT call `Career_SetCurrentCompetitionSlot` — that helper is
 * destructive (resets slot state from the static comp template,
 * losing the loaded season's progress). Here we only re-establish
 * the pointer values; the slot state itself is already correctly
 * loaded from the .CAR.
 *
 * National-selector careers populate these pointers via
 * `careerCupPickList` instead of competitionsTable. The pickList is
 * inside the saved overlay, so its entries are stale too — rebuild
 * via `Career_SetCupPickList_ForSelectedRegion(0)` first. */
static void Rebuild_CompetitionSlotPointers(void)
{
    /* Country / pickList tables are `Competition *[]` — 8 bytes per
     * entry on 64-bit. The Amiga path walked them as `uint32_t *`
     * which only read half a pointer per slot. Use `void **` so the
     * stride is correct on both targets. Sentinels stored as
     * `(Competition *)-1` / `-2` compare via (uintptr_t)X == -1u /
     * -2u — both halves of the pointer are all-ones on Amiga and
     * zero-extend identically on 64-bit. */
    void **compTablePtr;

    if (_isNationalSelector != 0) {
        Career_SetCupPickList_ForSelectedRegion(0);
        compTablePtr = (void **)&_careerCupPickList;
    } else {
        uint16_t tableIdx = (_selTeamNumber == 0xFFFF)
            ? SaveJson_GetLoadedPrimaryCountryCode()
            : (_selTeamNumber >> 8);
        compTablePtr = (void **)_competitionsTable[tableIdx];
    }
    if (!compTablePtr)
        return;

    /* Slot 0 (primary): always present. */
    _currentCompetitionPtr = (uint8_t *)*compTablePtr++;
    /* One filler slot after the primary entry (the `(Competition *)-2`
     * sentinel in country tables like albaniaTable). */
    compTablePtr++;

    /* Slot 1 (secondary): may be -1 sentinel. */
    if ((uintptr_t)*compTablePtr != (uintptr_t)-1) {
        _secondaryCompetitionPtr = (uint8_t *)*compTablePtr++;
        if ((uintptr_t)*compTablePtr != (uintptr_t)-1) {
            _tertiaryCompetitionPtr = (uint8_t *)*compTablePtr;
        } else {
            _tertiaryCompetitionPtr = 0;
        }
    } else {
        _secondaryCompetitionPtr = 0;
        _tertiaryCompetitionPtr = 0;
    }

    /* Slot 4 (DIY) — derive from primary comp's DIY link. Mirrors
     * `DIY_SelectCompetitionSlot4FromCurrentDivision`. The diyEntry
     * payload is two pointers (diy comp ptr + diy table ptr) — 8 B
     * stride on 64-bit. Use `void **` read instead of the old
     * `*(uint32_t *)` which truncated. */
    _diyCompetitionPtr = 0;
    _diyCompetitionTablePtr = (uint32_t *)0;
    if (_currentCompetitionPtr != 0) {
        uint8_t *primary = (uint8_t *)_currentCompetitionPtr;
        if (((Competition *)primary)->type == 0) {
            uint16_t off = _currentDivisionIndex * 6;
            uint8_t *entry = primary + off;
            uint8_t diyOffset = entry[0x12];
            if (diyOffset != 0) {
                void **diyEntry = (void **)(entry + 0x12 + diyOffset);
                _diyCompetitionPtr = (uint8_t *)diyEntry[0];
                _diyCompetitionTablePtr = (uint32_t *)diyEntry[1];
            }
        }
    }

    /* Slot 3 (quaternary, euro cups) — re-derive from the SAVED euro-cup
     * slot CONTEXT, not from quaternaryCompetitionPtr.
     *
     * The JSON save format no longer stores quaternaryCompetitionPtr (it is
     * a pointer-to-disk field). The previous gate `quaternaryCompetitionPtr
     * != 0` therefore read the stale pre-load value — 0 on a fresh-start
     * load — and left an ACTIVE euro-cup slot inactive. The careerMenu then
     * draws the slot-3 schedule entry, CareerGameListCommon sets
     * compInfo = quaternaryCompetitionPtr = NULL, and `compInfo[5]` SIGSEGVs
     * (Playaveli's CLAUDE.CAR repro, in CareerGameListCommon menus.c:6687).
     *
     * Reliable saved signal: the slot-3 context (euroCupsCompetitionContext)
     * is persisted and restored on load. Its competitionType (+0x00) and
     * isActive (+0x02) are 2/1 for an active European Cup (type-2 group
     * tournament) and 1/1 for the Cup Winners'/UEFA cup (type-1); an
     * inactive slot reads back all-zero. euroCupSlotIndex (also saved)
     * selects which of the three cups; we then VALIDATE that the saved
     * context's type agrees with that cup, so a stale euroCupSlotIndex (it
     * is only written on qualification) or a stale context can't activate a
     * mismatched slot — the original FindTeamInDIY-panic concern. */
    {
        uint8_t *euroCup3 = 0;
        if (_euroCupSlotIndex == 0)      euroCup3 = _euroCupCopy;        /* type 2 */
        else if (_euroCupSlotIndex == 1) euroCup3 = _cupWinnersCupCopy;  /* type 1 */
        else if (_euroCupSlotIndex == 2) euroCup3 = _uefaCupCopy;        /* type 1 */
        else if (_euroCupSlotIndex == 3) {                              /* type 2 (Copa) */
            /* Copa Libertadores shares the quaternary slot (a CONMEBOL club is in
             * none of the euro cups). Re-derive its *Copy so a saved South-American
             * career re-loads with Copa still interactive. The same type-match +
             * Career_TeamInLoadedEuroSlot validation below applies (Copa is type 2,
             * like the European Cup). */
            extern Competition *Career_CopaWorkingCopy(void);
            euroCup3 = (uint8_t *)Career_CopaWorkingCopy();
        }

        DIYCompetition *q3 = (DIYCompetition *)_careerCompetitionSlotPtrTable[3];
        /* Participation gate: euroCupSlotIndex is only written on qualification
         * (Career_SelectEuroCupSlotIfTeamParticipates) and is SAVED, so it can
         * be stale from a prior season when the club DID qualify. The euro cup
         * is contested every season by the qualifying clubs, so its slot-3
         * context can be active + type-matching while the player's club is NOT
         * in it. Without the participation check the fixup re-activates slot 3
         * for a non-participant, and ShowChairmanOpinionAboutTheSeason then
         * calls FindTeamInDIY(selTeamNumber, euroCupsCompetitionContext) which
         * hard-panics 0x6006 for a club coach (team not found). Mirror the
         * season-init enrolment: only activate if the club is actually in the
         * cup's team list. (gdb-confirmed 2026-06-10: club 5141 in SERIE A, euro
         * slot held EUROPEAN CHAMPIONS CUP whose Italian entrant is 5142, stale
         * euroCupSlotIndex=0.) */
        /* Gate on the LOADED BUFFER's team list (q3), NOT the static euroCupCopy
         * template. euroCupCopy is reconstructed from its static template on load
         * (default teams, NOT the saved season's qualifiers), so
         * Career_TeamInEuroCup(euroCup3) gives a FALSE NEGATIVE for a club that's
         * in the saved season's cup but not the default template — leaving
         * quaternaryCompetitionPtr NULL and crashing the cached slot-3 game-list
         * row in CareerGameListCommon on compInfo[5]. The saved slot-3 buffer is
         * the season's authoritative field; Career_TeamInLoadedEuroSlot checks it
         * with the same search FindTeamInDIY uses (gate-pass => no 0x6006). euroCup3
         * (the static record) is still the pointer TARGET — it carries the name
         * table the game list reads. */
        if (euroCup3 != 0 && q3->isActive != 0 &&
            q3->competitionType == ((Competition *)euroCup3)->type &&
            Career_TeamInLoadedEuroSlot(q3)) {
            _quaternaryCompetitionPtr = euroCup3;
        } else {
            _quaternaryCompetitionPtr = 0;  /* slot 3 inactive / club not in it this season */
        }
    }
}

/* Step 5 helper: zero the four *CompStringOffset fields on load.
 *
 * These are stale absolute pointers into the strings table (saved
 * verbatim from the saving binary's runtime address space). The only
 * consumer is `InitializeNewSeason` which copies them into the
 * SeasonInformations buffer at offsets 0x16/0x1A/0x1E/0x22 to display
 * the previous season's competition names on the season-end summary.
 * If we zero them, the summary may show empty strings for the prior
 * season's results, but no crash from dereferencing a stale pointer.
 *
 * Future improvement: re-derive each from its slot's compId by
 * looking up the comp record's `text[0]` field (same source as the
 * cup-record nameSource). Deferred until we have a save that exposes
 * a visible regression here. */
static void Rebuild_CompStringOffsets(void)
{
    _primaryCompStringOffset    = 0;
    _secondaryCompStringOffset  = 0;
    _tertiaryCompStringOffset   = 0;
    _quaternaryCompStringOffset = 0;
}

__attribute__((used, externally_visible))
void RebuildPointersAfterLoad(void)
{
    /* Step 1: refresh careerCompetitionSlotPtrTable[0..4]. Same five
     * lines as `InitNewSeason` lines 1165-1169 — but this time after
     * load, so the saved (now stale) pointer values get replaced with
     * the current run's competition-buffer addresses. */
    _careerCompetitionSlotPtrTable[0] = (uint8_t *)&_primaryCompetitionBuf;
    _careerCompetitionSlotPtrTable[1] = (uint8_t *)&_competitionContextBuf;
    _careerCompetitionSlotPtrTable[2] = (uint8_t *)&_tertiaryCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[3] = (uint8_t *)&_euroCupsCompetitionContextBuf;
    _careerCompetitionSlotPtrTable[4] = (uint8_t *)&_diyCompetitionContextBuf;

    /* Step 1b: re-derive each slot buffer's DIYCompetition.namePtr (+0x28).
     *
     * namePtr is a pointer-to-disk field: at save time it holds the
     * SAVING binary's absolute address of the division/cup name string
     * (e.g. divisionStringTable[d7], assigned in InitDIYLeagueFromRecord
     * diy_competition.c:8869 / InitDIYCupFromRecord:8924). The JSON save
     * stores it as raw bytes (primary_name_ptr), so after a cross-target
     * load it's a foreign address — on the Amiga a 64-bit-SDL value like
     * 0x1_004A70B8 truncates to a ~garbage 32-bit pointer. It then flows
     * into SeasonInformations.leagueStrPtr (line ~1067 below) and is
     * dereferenced by the season-recap renderer (statistics.c:2184,
     * StringCopy/RenderCompLine), which walks it to a NUL — off a wild
     * address that smashes the stack (observed: wild PC + "SWOS2" string
     * bytes on the stack walking past the comp buffer).
     *
     * Re-point it at the slot's OWN competitionName[36] buffer (+0x04),
     * which the same init code already filled with the full display name
     * ("GERMAN 1. LIGA"). That's valid, cross-target-stable memory in the
     * just-loaded buffer, so the recap renders and nothing derefs a
     * foreign address.
     *
     * LIMITATION (deferred): the original namePtr was the *bare* division
     * name ("1. LIGA") and is also tested by IDENTITY (== aFriendly,
     * diy_competition.c:2930/3223) and by ADDRESS RANGE (aEuropeanTour..
     * aOceaniaTour, career_status.c:3774) to classify friendlies/tours.
     * Pointing at competitionName loses those — fine for ordinary
     * league/cup (not a friendly, not a tour, and the recap shows the
     * country-prefixed name), but a friendly/tour career would
     * mis-classify. The full fix is a SYMBOLIC save (table-id + division
     * index, restored to the original static-string address) so identity
     * and range survive; see project_primary_comp_block_handoff. This
     * repoint unblocks the common case without that larger change. */
    for (int _slot = 0; _slot < 5; _slot++) {
        DIYCompetition *c = (DIYCompetition *)_careerCompetitionSlotPtrTable[_slot];
        if (c != NULL && c->namePtr != NULL)
            c->namePtr = (uint8_t *)c->competitionName;
    }

    /* Step 2: repopulate diyCompetitionStart (the runtime working
     * struct outside the careerFileBuffer overlay) from the primary
     * slot's saved bytes. Without this, the upcoming-match list
     * shows fixtures generated against whatever competition the
     * previous in-memory state held — typically the player team
     * "playing" every other team at home, with 00'00 kickoff times,
     * because the league fixture order/times live in
     * diyCompetitionStart and not in the saved overlay slot.
     *
     * VIEW SQUADS already worked without this step because that path
     * goes through `Career_OpenCompetitionSlotMenuAndApplyChanges`,
     * which calls Career_LoadCompetitionSlotStateFromBuffer itself.
     * The Play Match / Next Match menus skip that helper, so we
     * have to populate diyCompetitionStart up-front at load time. */
    Career_LoadCompetitionSlotStateFromBuffer((uint8_t *)&_primaryCompetitionBuf);

    /* Step 3: walk careerContests and rewrite each *Copy record's
     * embedded nameSource pointer at (record + 5 + nameOffset).
     * Without this, `InitDIYLeagueFromRecord` SIGSEGVs on a stale
     * string pointer when auto-advancing euroCupCopy /
     * cupWinnersCupCopy / uefaCupCopy after a cross-build load. */
    Rebuild_CupRecordNameSourcePointers();

    /* Step 4: re-derive top-level competition slot pointers
     * (`currentCompetitionPtr` / `secondaryCompetitionPtr` /
     * `tertiaryCompetitionPtr` / `quaternaryCompetitionPtr` /
     * `diyCompetitionPtr` / `diyCompetitionTablePtr`) from
     * competitionsTable + selTeamNumber + euroCupSlotIndex. Saved
     * values were stale BSS addresses from the saving build. */
    Rebuild_CompetitionSlotPointers();

    /* Step 5: zero the four *CompStringOffset fields. They held
     * stale absolute pointers into the strings table; downstream
     * consumers (InitializeNewSeason → SeasonInformations) will
     * tolerate NULL by displaying empty text. */
    Rebuild_CompStringOffsets();
}

/* ================================================================
 * Chunk 198: Career_ViewOrEditCompetitionSlot  (SWOS2.s line 32242)
 *
 * Views or edits a competition slot: maps the competition pointer
 * to its career copy via careerContests table, handles special
 * europeanChampionships/worldCup replacements, then determines which
 * slot owns the competition and shows appropriate menu.
 * If the competition isn't in any active slot, auto-advances it.
 *
 * in:  a0 = competition pointer, d0 = divisionIndex/selection
 * ================================================================ */
extern uint8_t _europeanChampionships[];
extern uint8_t _europeanChampionships2Copy[];
extern uint8_t _worldCup[];
extern uint8_t _worldCup2Copy[];
/* nationalTeamCompetitionsAndQualifications1+0x96 effectively points
 * 6 bytes before careerContests[0] — the loop skips 6 bytes then reads
 * 4 bytes per iteration, matching the 10-byte careerContests entries. */

extern void ShowDiyLeaguesTournamentMenu(uint16_t type, uint16_t viewOnly, uint16_t focusOverride);
extern void ShowDIYCupMenuWithParams(uint16_t arg0, uint16_t arg1, uint16_t arg2);
extern void DIY_EditCompetitionSlot(uint16_t paramA, uint16_t paramB, uint16_t paramC);
extern void SaveSelectedTeams(void);
extern void RestoreSelectedTeams(void);
extern void Career_LoadCompetitionSlotStateFromBuffer(uint8_t *src);
extern void Career_AutoAdvanceInternationalCompetitionFixtures(uint8_t *record, uint16_t index);
extern void Career_AutoAdvanceCupCompetitionFromRecord(uint8_t *record);
extern void Career_AutoAdvanceLeagueCompetitionFromRecord(uint8_t *record);

__attribute__((used, externally_visible))
void Career_ViewOrEditCompetitionSlot(uint8_t *comp, uint16_t sel)
{
    /* Step 1: Map competition pointer to its career copy via the
     * typed CompetitionCareer[] — entry stride differs by bitness
     * (10 B on 32-bit, 18 B on 64-bit), so iterate as the typed
     * array instead of advancing tbl by a literal byte count. */
    extern CompetitionCareer _careerContests[];
    for (CompetitionCareer *e = _careerContests;
         (uintptr_t)e->competition != 0xFFFFFFFFu; e++) {
        if ((uint8_t *)e->competition == comp) {
            comp = (uint8_t *)e->competitionCopy;
            break;
        }
    }

    /* Step 2: Special replacements for europeanChampionships and worldCup.
     * compId 22/4 appear only on these templates (the "2" copies are 21/3),
     * so by-id is exactly equivalent to the old template-address compare. */
    uint8_t cid = comp ? comp_id(comp) : 0xFFu;
    if (cid == COMPID_europeanChampionships)
        comp = _europeanChampionships2Copy;
    if (cid == COMPID_worldCup)
        comp = _worldCup2Copy;

    /* Step 3: Find which slot owns this competition */
    uint8_t *slotCtx = (uint8_t *)&_primaryCompetitionBuf;

    if (comp == (uint8_t *)_currentCompetitionPtr) {
        if (sel == _currentDivisionIndex)
            goto found_slot;
    }

    slotCtx = (uint8_t *)&_competitionContextBuf;
    if (comp == (uint8_t *)_secondaryCompetitionPtr)
        goto found_slot;

    slotCtx = (uint8_t *)&_tertiaryCompetitionContextBuf;
    if (comp == (uint8_t *)_tertiaryCompetitionPtr)
        goto found_slot;

    slotCtx = (uint8_t *)&_euroCupsCompetitionContextBuf;
    if (comp == (uint8_t *)_quaternaryCompetitionPtr)
        goto found_slot;

    /* Not in any active slot. Trace every stage to identify remaining crash. */
    {
        FILE *log = fopen("swos_division_debug.log", "a");
        int compType = comp ? ((Competition *)comp)->type : -1;
        int divisions = (comp && compType == 0) ? ((CompLeagueHeader *)comp)->divisionsCount : -1;
        if (log) {
            fprintf(log, "VIEW enter compId=%u type=%d selected=%u divisions=%d active=%u\n",
                    comp ? (unsigned)comp_id(comp) : 255u, compType,
                    (unsigned)sel, divisions, (unsigned)_currentDivisionIndex);
            fclose(log);
        }
        /* Reject invalid division indices before indexing divisions[]. */
        if (!comp || (compType == 0 && (divisions <= 0 || divisions > 4 || sel >= (unsigned)divisions))) {
            log = fopen("swos_division_debug.log", "a");
            if (log) { fprintf(log, "VIEW invalid competition/division, refusing\n"); fclose(log); }
            return;
        }
    }
    SaveSelectedTeams();
    {
        int8_t compType = ((Competition *)comp)->type;
        FILE *log = fopen("swos_division_debug.log", "a");
        if (log) { fprintf(log, "VIEW before advance type=%d selected=%u\n", (int)compType, (unsigned)sel); fclose(log); }
        if (compType == 1) {
            Career_AutoAdvanceCupCompetitionFromRecord(comp);
        } else if (compType == 0) {
            Career_AutoAdvanceInternationalCompetitionFixtures(comp, sel);
        } else {
            Career_AutoAdvanceLeagueCompetitionFromRecord(comp);
        }
        log = fopen("swos_division_debug.log", "a");
        if (log) { fprintf(log, "VIEW after advance\n"); fclose(log); }
    }
    RestoreSelectedTeams();
    {
        FILE *log = fopen("swos_division_debug.log", "a");
        if (log) { fprintf(log, "VIEW after restore\n"); fclose(log); }
    }
    return;

found_slot:
    Career_LoadCompetitionSlotStateFromBuffer(slotCtx);
    {
        uint8_t compType = slotCtx[1];
        if (compType == 1) {
            ShowDIYCupMenuWithParams(1, 1, 0);
        } else if (compType == 2) {
            DIY_EditCompetitionSlot(1, 1, 0);
        } else {
            ShowDiyLeaguesTournamentMenu(1, 1, 0);
        }
    }
}

/* Manual AI transfer action. Available in all seasons, including season one.
 * In addition to the once-per-season automatic pass in
 * ContinueCareerNextSeason(), each explicit menu click requests a batch.
 */
__attribute__((used, externally_visible))
void SWOS_AI_RunTransfersMenu(void)
{
    SWOS_AI_LOG("[SWOS AI Button] CLICK: season=%u, before records=%u\n",
                (unsigned)_careerSeasonsCompleted, (unsigned)_numForeignMarketPlayers2);
    SWOS_AI_RealTransferTest();
    SWOS_AI_LOG("[SWOS AI Button] DONE: season=%u, after records=%u\n",
                (unsigned)_careerSeasonsCompleted, (unsigned)_numForeignMarketPlayers2);
}
