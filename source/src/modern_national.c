#include "modern_national.h"
#include "modern_career_runtime.h"
#include "career_teams.h"
#include "endian_helpers.h"
#include "teams.h"

#include <stdlib.h>
#include <string.h>

#define NATIONAL_MATCH_SIZE 16


typedef struct NationalCandidate {
    PlayerFile player;
    uint16_t club;
    uint16_t slot;
    char clubName[17];
    char id[MODERN_ID_LEN];
    unsigned score;
    unsigned age;
} NationalCandidate;

static NationalCandidate *candidates;
static size_t nationStart[257];

/* The base national TEAM.080–085 keep their competition IDs. The
 * 2026/27 roster assigns separate codes to former duplicate nationalities
 * such as Nigeria/Niger, India/Indonesia and Iran/Iraq. File 255 means
 * no domestic club file. */
static const NationalTeamRecord nationRecords[][49] = {
    {
        { 80, 0, 0, 0, 0 }, /* ALBANIA */
        { 80, 1, 41, 52, 0 }, /* ARMENIA */
        { 80, 2, 1, 1, 0 }, /* AUSTRIA */
        { 80, 3, 43, 53, 0 }, /* AZERBAIJAN */
        { 80, 4, 38, 76, 0 }, /* BELARUS */
        { 80, 5, 2, 2, 0 }, /* BELGIUM */
        { 80, 6, 3, 3, 0 }, /* BULGARIA */
        { 80, 7, 4, 4, 0 }, /* CROATIA */
        { 80, 8, 5, 5, 0 }, /* CYPRUS */
        { 80, 9, 6, 6, 0 }, /* CZECH REPUBLIC */
        { 80, 10, 7, 7, 0 }, /* DENMARK */
        { 80, 11, 8, 8, 0 }, /* ENGLAND */
        { 80, 12, 9, 10, 0 }, /* ESTONIA */
        { 80, 13, 10, 11, 0 }, /* FAROE ISLES */
        { 80, 14, 11, 12, 0 }, /* FINLAND */
        { 80, 15, 12, 13, 0 }, /* FRANCE */
        { 80, 16, 44, 61, 0 }, /* GEORGIA */
        { 80, 17, 13, 14, 0 }, /* GERMANY */
        { 80, 18, 14, 15, 0 }, /* GREECE */
        { 80, 19, 23, 25, 0 }, /* HOLLAND */
        { 80, 20, 15, 16, 0 }, /* HUNGARY */
        { 80, 21, 16, 17, 0 }, /* ICELAND */
        { 80, 22, 17, 19, 0 }, /* ISRAEL */
        { 80, 23, 18, 20, 0 }, /* ITALY */
        { 80, 24, 19, 21, 0 }, /* LATVIA */
        { 80, 25, 49, 90, 0 }, /* LIECHTENSTEIN */
        { 80, 26, 20, 22, 0 }, /* LITHUANIA */
        { 80, 27, 21, 23, 0 }, /* LUXEMBOURG */
        { 80, 28, 47, 93, 0 }, /* MACEDONIA */
        { 80, 29, 22, 24, 0 }, /* MALTA */
        { 80, 30, 50, 92, 0 }, /* MOLDOVA */
        { 80, 31, 24, 26, 0 }, /* NORTHERN IRELAND */
        { 80, 32, 25, 27, 0 }, /* NORWAY */
        { 80, 33, 26, 28, 0 }, /* POLAND */
        { 80, 34, 27, 29, 0 }, /* PORTUGAL */
        { 80, 35, 46, 18, 0 }, /* REPUBLIC IRELAND */
        { 80, 36, 28, 30, 0 }, /* ROMANIA */
        { 80, 37, 29, 31, 0 }, /* RUSSIA */
        { 80, 38, 30, 32, 0 }, /* SAN MARINO */
        { 80, 39, 31, 33, 0 }, /* SCOTLAND */
        { 80, 40, 39, 78, 0 }, /* SLOVAKIA */
        { 80, 41, 32, 34, 0 }, /* SLOVENIA */
        { 80, 42, 40, 47, 0 }, /* SPAIN */
        { 80, 43, 33, 36, 0 }, /* SWEDEN */
        { 80, 44, 45, 37, 0 }, /* SWITZERLAND */
        { 80, 45, 34, 38, 0 }, /* TURKEY */
        { 80, 46, 35, 39, 0 }, /* UKRAINE */
        { 80, 47, 36, 40, 0 }, /* WALES */
        { 80, 48, 37, 41, 0 }, /* YUGOSLAVIA */
    },
    {
        { 81, 0, 80, 42, 0 }, /* ALGERIA */
        { 81, 1, 102, 255, 0 }, /* ANGOLA */
        { 81, 2, 111, 255, 0 }, /* BENIN */
        { 81, 3, 82, 255, 0 }, /* BOTSWANA */
        { 81, 4, 83, 255, 0 }, /* BURKINA FASO */
        { 81, 5, 84, 255, 0 }, /* BURUNDI */
        { 81, 6, 94, 255, 0 }, /* CAMEROON */
        { 81, 7, 112, 255, 0 }, /* CONGO */
        { 81, 8, 105, 59, 0 }, /* EGYPT */
        { 81, 9, 108, 255, 0 }, /* ETHIOPIA */
        { 81, 10, 109, 255, 0 }, /* GABON */
        { 81, 11, 88, 79, 0 }, /* GHANA */
        { 81, 12, 113, 255, 0 }, /* GUINEA */
        { 81, 13, 90, 255, 0 }, /* IVORY COAST */
        { 81, 14, 99, 255, 0 }, /* KENYA */
        { 81, 15, 85, 255, 0 }, /* LESOTHO */
        { 81, 16, 97, 255, 0 }, /* LIBERIA */
        { 81, 17, 117, 255, 0 }, /* MALAWI */
        { 81, 18, 92, 255, 0 }, /* MALI */
        { 81, 19, 115, 91, 0 }, /* MOROCCO */
        { 81, 20, 98, 255, 0 }, /* MOZAMBIQUE */
        { 81, 21, 182, 255, 0 }, /* NIGER */
        { 81, 22, 107, 255, 0 }, /* NIGERIA */
        { 81, 23, 89, 255, 0 }, /* SENEGAL */
        { 81, 24, 110, 255, 0 }, /* SIERRA LEONE */
        { 81, 25, 81, 69, 0 }, /* SOUTH AFRICA */
        { 81, 26, 100, 255, 0 }, /* SUDAN */
        { 81, 27, 101, 255, 0 }, /* SWAZILAND */
        { 81, 28, 106, 255, 0 }, /* TANZANIA */
        { 81, 29, 103, 255, 0 }, /* TOGO */
        { 81, 30, 91, 97, 0 }, /* TUNISIA */
        { 81, 31, 96, 255, 0 }, /* UGANDA */
        { 81, 32, 87, 255, 0 }, /* ZAMBIA */
        { 81, 33, 104, 255, 0 }, /* ZIMBABWE */
    },
    {
        { 82, 0, 68, 43, 0 }, /* ARGENTINA */
        { 82, 1, 69, 45, 0 }, /* BOLIVIA */
        { 82, 2, 70, 46, 0 }, /* BRAZIL */
        { 82, 3, 71, 48, 0 }, /* CHILE */
        { 82, 4, 72, 49, 0 }, /* COLOMBIA */
        { 82, 5, 73, 50, 0 }, /* ECUADOR */
        { 82, 6, 74, 64, 0 }, /* PARAGUAY */
        { 82, 7, 79, 65, 0 }, /* PERU */
        { 82, 8, 76, 71, 0 }, /* URUGUAY */
        { 82, 9, 77, 77, 0 }, /* VENEZUELA */
    },
    {
        { 83, 0, 61, 255, 0 }, /* BERMUDA */
        { 83, 1, 64, 56, 0 }, /* CANADA */
        { 83, 2, 51, 58, 0 }, /* COSTA RICA */
        { 83, 3, 52, 51, 0 }, /* EL SALVADOR */
        { 83, 4, 53, 68, 0 }, /* GUATEMALA */
        { 83, 5, 78, 255, 0 }, /* GUYANA */
        { 83, 6, 54, 70, 0 }, /* HONDURAS */
        { 83, 7, 62, 255, 0 }, /* JAMAICA */
        { 83, 8, 56, 60, 0 }, /* MEXICO */
        { 83, 9, 60, 255, 0 }, /* NICARAGUA */
        { 83, 10, 57, 95, 0 }, /* PANAMA */
        { 83, 11, 67, 255, 0 }, /* ST. VINCENT */
        { 83, 12, 75, 66, 0 }, /* SURINAM */
        { 83, 13, 63, 255, 0 }, /* TRINIDAD-TOBAGO */
        { 83, 14, 58, 73, 0 }, /* UNITED STATES */
    },
    {
        { 84, 0, 197, 255, 0 }, /* BAHRAIN */
        { 84, 1, 121, 255, 0 }, /* BANGLADESH */
        { 84, 2, 122, 255, 0 }, /* BRUNEI */
        { 84, 3, 139, 57, 0 }, /* CHINA */
        { 84, 4, 198, 255, 0 }, /* HONG KONG */
        { 84, 5, 120, 75, 0 }, /* INDIA */
        { 84, 6, 200, 255, 0 }, /* INDONESIA */
        { 84, 7, 128, 74, 0 }, /* IRAN */
        { 84, 8, 123, 255, 0 }, /* IRAQ */
        { 84, 9, 118, 55, 0 }, /* JAPAN */
        { 84, 10, 124, 255, 0 }, /* JORDAN */
        { 84, 11, 133, 255, 0 }, /* KUWAIT */
        { 84, 12, 134, 255, 0 }, /* LAOS */
        { 84, 13, 130, 255, 0 }, /* MALAYSIA */
        { 84, 14, 141, 255, 0 }, /* MAURITIUS */
        { 84, 15, 142, 255, 0 }, /* MYANMAR */
        { 84, 16, 135, 255, 0 }, /* NORTH KOREA */
        { 84, 17, 136, 255, 0 }, /* OMAN */
        { 84, 18, 137, 255, 0 }, /* PAKISTAN */
        { 84, 19, 143, 255, 0 }, /* PAPUA NEW GUINEA */
        { 84, 20, 138, 255, 0 }, /* PHILIPPINES */
        { 84, 21, 146, 96, 0 }, /* QATAR */
        { 84, 22, 131, 89, 0 }, /* SAUDI ARABIA */
        { 84, 23, 140, 255, 0 }, /* SINGAPORE */
        { 84, 24, 127, 87, 0 }, /* SOUTH KOREA */
        { 84, 25, 125, 255, 0 }, /* SRI LANKA */
        { 84, 26, 126, 255, 0 }, /* SYRIA */
        { 84, 27, 119, 67, 0 }, /* TAIWAN */
        { 84, 28, 147, 98, 0 }, /* UNITED ARAB EMIS */
        { 84, 29, 129, 255, 0 }, /* VIETNAM */
        { 84, 30, 132, 255, 0 }, /* YEMEN */
    },
    {
        { 85, 0, 148, 44, 0 }, /* AUSTRALIA */
        { 85, 1, 150, 255, 0 }, /* FIJI */
        { 85, 2, 149, 62, 0 }, /* NEW ZEALAND */
        { 85, 3, 151, 255, 0 }, /* SOLOMON ISLANDS */
    },
};

int ModernNational_StartPicker;

const NationalTeamRecord *ModernNational_Record(uint16_t team)
{
    unsigned file = team >> 8;
    unsigned ordinal = team & 255;
    if (file < 80 || file > 85 || ordinal >= 49 || !ModernRuntime_Enabled())
        return NULL;
    const NationalTeamRecord *record = &nationRecords[file - 80][ordinal];
    return record->countryNumber == file ? record : NULL;
}

int ModernNational_PlayerEligible(const ModernRosterPlayer *p)
{
    unsigned char initial = (unsigned char)p->swos.playerName[0];
    return initial && initial != 255 &&
           !(p->swos.positionAndFace & (PF_LOAN | PF_TRIALIST)) &&
           (!p->is_reserve || (p->age >= 18 &&
            (uint8_t)p->swos.form != PLAYER_FORM_RESERVE &&
            (uint8_t)p->swos.form != PLAYER_FORM_TRIALIST));
}

static unsigned strength(const PlayerFile *p)
{
    unsigned vh = (uint8_t)p->velocityHeading;
    unsigned tc = (uint8_t)p->tacklingControl;
    unsigned sf = (uint8_t)p->speedFinishing;
    return (uint8_t)p->price * 64u + ((uint8_t)p->passing & 15) +
           (vh >> 4) + (vh & 15) + (tc >> 4) + (tc & 15) +
           (sf >> 4) + (sf & 15);
}

static int compare_candidates(const void *a, const void *b)
{
    const NationalCandidate *left = a, *right = b;
    if (left->score != right->score)
        return left->score > right->score ? -1 : 1;
    return strcmp(left->id, right->id);
}

void ModernNational_Refresh(const ModernWorld *world)
{
    extern uint16_t _teamsLoaded;
    size_t insert[256] = {0};

    free(candidates);
    candidates = NULL;
    memset(nationStart, 0, sizeof nationStart);
    _teamsLoaded = 0;

    for (size_t i = 0; i < world->count; ++i) {
        const ModernRoster *club = &world->clubs[i];
        for (size_t j = 0; j < club->count; ++j)
            if (ModernNational_PlayerEligible(&club->players[j]))
                ++insert[(uint8_t)club->players[j].swos.nationality];
    }
    for (unsigned i = 0; i < 256; ++i) {
        nationStart[i + 1] = nationStart[i] + insert[i];
        insert[i] = nationStart[i];
    }
    if (!nationStart[256])
        return;
    candidates = malloc(nationStart[256] * sizeof *candidates);
    if (!candidates) {
        memset(nationStart, 0, sizeof nationStart);
        return;
    }
    for (size_t i = 0; i < world->count; ++i) {
        const ModernRoster *club = &world->clubs[i];
        uint16_t team = 0xFFFF;
        ModernRuntime_ActiveClub(club, &team);
        for (size_t j = 0; j < club->count; ++j) {
            const ModernRosterPlayer *player = &club->players[j];
            if (!ModernNational_PlayerEligible(player))
                continue;
            NationalCandidate *entry = &candidates[insert[(uint8_t)player->swos.nationality]++];
            entry->player = player->swos;
            entry->club = team;
            entry->slot = (uint16_t)j;
            memcpy(entry->clubName, club->header.teamName, sizeof entry->clubName - 1);
            entry->clubName[sizeof entry->clubName - 1] = '\0';
            memcpy(entry->id, player->id, sizeof entry->id);
            entry->score = strength(&entry->player);
            entry->age = player->age;
        }
    }
    for (unsigned nat = 0; nat < 256; ++nat)
        qsort(candidates + nationStart[nat], nationStart[nat + 1] - nationStart[nat],
              sizeof *candidates, compare_candidates);
}

int ModernNational_CanManage(uint16_t team)
{
    const NationalTeamRecord *record = ModernNational_Record(team);
    if (!record)
        return 0;
    unsigned nat = record->nationalityCode;
    unsigned keepers = 0, outfield = 0;
    for (size_t i = nationStart[nat]; i < nationStart[nat + 1]; ++i) {
        if (candidates[i].player.positionAndFace & PF_POSITION_MASK)
            ++outfield;
        else
            ++keepers;
    }
    return keepers && outfield >= (keepers >= 2 ? 14 : 15);
}

static unsigned role_group(unsigned role)
{
    return role == 0 ? 0 : role <= 3 ? 1 : role <= 6 ? 2 : 3;
}

static const NationalCandidate *best_remaining(unsigned nat, unsigned role, unsigned pass,
                                               const NationalCandidate *const *selected,
                                               unsigned limit)
{
    for (size_t i = nationStart[nat]; i < nationStart[nat + 1]; ++i) {
        const NationalCandidate *player = &candidates[i];
        unsigned natural = player->player.positionAndFace >> 5;
        if (((natural == 0) != (role == 0)) || (pass == 0 && natural != role) ||
            (pass == 1 && role_group(natural) != role_group(role)))
            continue;
        unsigned j = 0;
        while (j < limit && selected[j] != player)
            ++j;
        if (j == limit)
            return player;
    }
    return NULL;
}

static unsigned choose_squad(const NationalTeamRecord *record, unsigned tactic,
                             const NationalCandidate **selected, unsigned limit)
{
    /* Career+'s physical SWOS roles for the stock tactics, before the
     * squad screen remaps their displayed rows. */
    static const uint8_t roles[][11] = {
        {0,1,3,3,2,4,6,6,5,7,7}, {0,1,3,3,2,4,3,6,5,6,7},
        {0,1,3,3,2,4,6,6,5,6,7}, {0,1,3,3,2,4,3,6,5,7,7},
        {0,3,3,6,3,4,6,6,5,7,7}, {0,1,3,3,2,4,6,5,7,7,7},
        {0,1,3,3,2,6,6,7,7,7,7}, {0,3,3,3,4,6,6,5,7,7,7},
        {0,1,3,3,2,4,6,6,5,7,7}, {0,1,3,3,3,2,6,6,7,7,7},
        {0,3,3,3,4,6,6,5,7,7,7}, {0,1,3,3,3,2,4,6,6,5,7}
    };
    unsigned nat = record->nationalityCode;
    const uint8_t *formation = roles[tactic < sizeof roles / sizeof roles[0] ? tactic : 0];
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned slot = 0; slot < 11; ++slot)
            if (!selected[slot])
                selected[slot] = best_remaining(nat, formation[slot], pass, selected, limit);

    if (!selected[11])
        selected[11] = best_remaining(nat, 0, 0, selected, limit);
    for (unsigned slot = 11; slot < limit; ++slot)
        if (!selected[slot])
            selected[slot] = best_remaining(nat, 7, 2, selected, limit);

    unsigned count = 0;
    while (count < NATIONAL_MATCH_SIZE && selected[count])
        ++count;
    if (count != NATIONAL_MATCH_SIZE)
        return count;
    for (unsigned i = NATIONAL_MATCH_SIZE; i < limit; ++i)
        if (selected[i])
            selected[count++] = selected[i];
    return count;
}

static void copy_callup(PlayerFile *dst, const NationalCandidate *src, unsigned slot)
{
    *dst = src->player;
    dst->shirtNumber = (int8_t)(slot + 1);
    dst->positionAndFace &= (PF_POSITION_MASK | PF_FACE_MASK);
    dst->cardsInjuries = 0;
    dst->cachedMatchValue = 0;
    dst->form = 0;
    memset(dst->goalStats, 0, sizeof dst->goalStats);
}

int ModernNational_ProjectTeam(TeamFileEx *team)
{
    uint16_t id = PACK_BE_U16(team->team.countryNumber, team->team.teamOrdinal);
    const NationalTeamRecord *record = ModernNational_Record(id);
    if (!record || !candidates)
        return 0;
    const NationalCandidate *selected[NATIONAL_MATCH_SIZE] = {0};
    if (choose_squad(record, team->team.tactics, selected,
                     NATIONAL_MATCH_SIZE) != NATIONAL_MATCH_SIZE)
        return 0;
    if (record->nationalityCode == 37 &&
        !strncmp(team->team.teamName, "YUGOSLAVIA", sizeof team->team.teamName))
        strcpy(team->team.teamName, "SERBIA");
    for (unsigned i = 0; i < NATIONAL_MATCH_SIZE; ++i) {
        copy_callup(&team->players[i], selected[i], i);
        team->team.playerNumbers[i] = (uint8_t)i;
    }
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
    return 1;
}

static PlayerFile *managed_player(TeamFileEx *team, unsigned slot)
{
    return (PlayerFile *)((uint8_t *)team + sizeof(TeamFile) + slot * sizeof(PlayerFile));
}

/* National selections store a club and slot, plus a copy of the player.
 * A transfer or squad reorder can make that slot stale. Check the copied
 * identity first and use a unique name match only when the slot disagrees. */
static int same_callup(const PlayerFile *a, const PlayerFile *b)
{
    return a->nationality == b->nationality &&
        !strncmp((const char *)a->playerName, (const char *)b->playerName, sizeof a->playerName);
}

unsigned ModernNational_PlayerAge(ModernWorld *world, const TeamFileEx *team, unsigned slot)
{
    extern TeamFileEx _careerTeam;
    extern uint16_t _careerNumPlayers, _careerUnavailablePlayerLookupTable[];
    extern uint8_t _careerUnavailablePlayerNames[];

    int managed = team == &_careerTeam;
    if (slot >= (managed ? _careerNumPlayers : NATIONAL_MATCH_SIZE) ||
        slot >= MODERN_NATIONAL_SQUAD_SIZE)
        return 0;
    const PlayerFile *member = managed_player((TeamFileEx *)team, slot);
    if (!member->playerName[0] || (uint8_t)member->playerName[0] == 255)
        return 0;

    const char *clubName = "";
    if (managed) {
        uint16_t sourceTeam = _careerUnavailablePlayerLookupTable[2 * slot];
        unsigned sourceSlot = _careerUnavailablePlayerLookupTable[2 * slot + 1];
        clubName = (const char *)_careerUnavailablePlayerNames + 17 * slot;
        const ModernRoster *club = sourceTeam == 0xFFFF ? NULL :
            ModernRuntime_Roster(world, sourceTeam >> 8, sourceTeam & 255);
        if (club && sourceSlot < club->count &&
            (!*clubName || !strncmp(clubName, club->header.teamName, 16)) &&
            same_callup(member, &club->players[sourceSlot].swos))
            return club->players[sourceSlot].age;
    }

    unsigned nat = (uint8_t)member->nationality;
    const NationalCandidate *match = NULL, *clubMatch = NULL;
    unsigned matches = 0, clubMatches = 0;
    for (size_t i = nationStart[nat]; i < nationStart[nat + 1]; ++i) {
        const NationalCandidate *candidate = &candidates[i];
        if (!same_callup(member, &candidate->player))
            continue;
        match = candidate;
        ++matches;
        if (*clubName && !strncmp(clubName, candidate->clubName, 16)) {
            clubMatch = candidate;
            ++clubMatches;
        }
    }
    if (clubMatches == 1)
        return clubMatch->age;
    return matches == 1 ? match->age : 0;
}

void ModernNational_SnapshotCallups(ModernWorld *world,
    char ids[MODERN_NATIONAL_SQUAD_SIZE][MODERN_ID_LEN])
{
    extern TeamFileEx _careerTeam;
    extern uint16_t _careerUnavailablePlayerLookupTable[];
    extern uint8_t _careerUnavailablePlayerNames[];

    memset(ids, 0, MODERN_NATIONAL_SQUAD_SIZE * MODERN_ID_LEN);
    for (unsigned i = 0; i < MODERN_NATIONAL_SQUAD_SIZE; ++i) {
        const PlayerFile *member = managed_player(&_careerTeam, i);
        if (!member->playerName[0] || (uint8_t)member->playerName[0] == 255)
            continue;
        uint16_t team = _careerUnavailablePlayerLookupTable[i * 2];
        unsigned slot = _careerUnavailablePlayerLookupTable[i * 2 + 1];
        const char *clubName = (const char *)_careerUnavailablePlayerNames + i * 17;
        const ModernRoster *club = team == 0xFFFF ? NULL :
            ModernRuntime_Roster(world, team >> 8, team & 255);
        if (club && slot < club->count &&
            (!clubName[0] || !strncmp(clubName, club->header.teamName, 16)) &&
            same_callup(member, &club->players[slot].swos)) {
            memcpy(ids[i], club->players[slot].id, MODERN_ID_LEN);
            continue;
        }

        const ModernRosterPlayer *match = NULL, *clubMatch = NULL;
        unsigned matches = 0, clubMatches = 0;
        for (size_t j = 0; j < world->count; ++j) {
            club = &world->clubs[j];
            for (size_t k = 0; k < club->count; ++k) {
                const ModernRosterPlayer *player = &club->players[k];
                if (!same_callup(member, &player->swos))
                    continue;
                match = player;
                ++matches;
                if (clubName[0] && !strncmp(clubName, club->header.teamName, 16)) {
                    clubMatch = player;
                    ++clubMatches;
                }
            }
        }
        if (clubMatches == 1)
            memcpy(ids[i], clubMatch->id, MODERN_ID_LEN);
        else if (matches == 1)
            memcpy(ids[i], match->id, MODERN_ID_LEN);
    }
}

int ModernNational_PlayerCalledUp(uint16_t club, const PlayerFile *player, const char *clubName)
{
    extern TeamFileEx _careerTeam;
    extern uint16_t _careerUnavailablePlayerLookupTable[];
    extern uint8_t _careerUnavailablePlayerNames[];
    for (unsigned i = 0; i < MODERN_NATIONAL_SQUAD_SIZE; ++i) {
        const PlayerFile *member = managed_player(&_careerTeam, i);
        if (!member->playerName[0] || (uint8_t)member->playerName[0] == 255 ||
            member->nationality != player->nationality ||
            strncmp((const char *)member->playerName, (const char *)player->playerName,
                    sizeof member->playerName))
            continue;
        const char *memberClub = (const char *)_careerUnavailablePlayerNames + 17 * i;
        if ((club != 0xFFFF && _careerUnavailablePlayerLookupTable[2 * i] == club) ||
            !strncmp(memberClub, clubName, 16))
            return 1;
    }
    return 0;
}

/* Rebind only the manager's own selections. The refreshed candidate pool
 * contains surviving players at their current club, even after a transfer. */
void ModernNational_ReconcileCallups(
    const char ids[MODERN_NATIONAL_SQUAD_SIZE][MODERN_ID_LEN])
{
    extern TeamFileEx _careerTeam;
    extern uint8_t _nationalTeamNationality;
    extern uint16_t _careerUnavailablePlayerLookupTable[];
    extern uint8_t _careerUnavailablePlayerNames[];

    if (!candidates)
        return;

    uint32_t removeMask = 0;
    unsigned nat = _nationalTeamNationality;
    for (unsigned slot = 0; slot < MODERN_NATIONAL_SQUAD_SIZE; ++slot) {
        PlayerFile *player = managed_player(&_careerTeam, slot);
        if (!player->playerName[0] || (uint8_t)player->playerName[0] == 255)
            continue;

        player->cardsInjuries = 0;
        if (!ids[slot][0])
            continue;

        const NationalCandidate *match = NULL;
        for (size_t i = nationStart[nat]; i < nationStart[nat + 1]; ++i) {
            if (!strcmp(ids[slot], candidates[i].id)) {
                match = &candidates[i];
                break;
            }
        }
        if (!match) {
            removeMask |= UINT32_C(1) << slot;
            _careerUnavailablePlayerLookupTable[2 * slot + 1] = 0;
            memset(_careerUnavailablePlayerNames + 17 * slot, 0, 17);
            continue;
        }

        uint8_t shirt = (uint8_t)player->shirtNumber;
        copy_callup(player, match, slot);
        player->shirtNumber = (int8_t)shirt;
        _careerUnavailablePlayerLookupTable[2 * slot] = match->club;
        _careerUnavailablePlayerLookupTable[2 * slot + 1] = match->slot;
        memcpy(_careerUnavailablePlayerNames + 17 * slot, match->clubName, 17);
    }
    Career_RemoveInternationalPlayersByMask(removeMask);
    _careerTeam.team.playerValuesDirty = TEAM_VALUES_DIRTY;
}
