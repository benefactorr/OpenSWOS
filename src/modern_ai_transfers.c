/* Career+ AI transfers over active clubs and persistent player identities. */
#include "modern_career_runtime.h"
#include "career_teams.h"
#include "menu_dialog.h"
#include "teams.h"
#include "world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint16_t _selTeamNumber, _numSelectedTeams, _careerFileBuffer;
extern uint8_t _tmdFileBuffer[];

typedef struct TransferClub {
    ModernRoster *roster;
    uint8_t country, quota, completed;
    size_t next_domestic;
} TransferClub;

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state >> 8;
}

static unsigned transfer_value(const ModernRosterPlayer *player)
{
    if (player->swos.positionAndFace & PF_LOAN) return 0;
    unsigned price = (uint8_t)player->swos.price;
    return price <= 49 && player->swos.playerName[0] ? ModernPlayerTransferValueBase((uint8_t)price) : 0;
}

static int recently_transferred(const ModernRosterPlayer *player, uint32_t run)
{
    return player->last_ai_transfer && run - player->last_ai_transfer <= 2;
}

static int transfer_candidate(const ModernRosterPlayer *first, const ModernRosterPlayer *second,
                              unsigned value, int higher)
{
    if (((first->swos.positionAndFace ^ second->swos.positionAndFace) & 0xe0) != 0)
        return 0;
    unsigned other = transfer_value(second);
    return other * 100u >= value * 70u && other * 100u <= value * 130u &&
           (higher ? other > value : other < value);
}

static void refresh_teams(ModernWorld *world, TeamFileEx *teams, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        TeamFileEx *team = &teams[i];
        uint16_t id = (team->team.countryNumber << 8) | team->team.teamOrdinal;
        if (id != _selTeamNumber)
            ModernRuntime_ProjectOpponent(world, team);
    }
}

unsigned ModernAI_RunTransfers(uint32_t seed, int show_summary)
{
    ModernWorld *world = ModernWorld_Active();
    if (!ModernRuntime_Enabled() || !world->count ||
        !ModernRuntime_FlushAll(world)) return 0;

    TransferClub *clubs = calloc(world->count, sizeof *clubs);
    size_t *order = malloc(world->count * sizeof *order);
    if (!clubs || !order) {
        free(clubs);
        free(order);
        return 0;
    }

    size_t count = 0;
    size_t first_in_country[256] = {0};
    for (size_t i = 0; i < world->count; ++i) {
        ModernRoster *roster = &world->clubs[i];
        uint16_t team;
        if (!roster->count || !ModernRuntime_ActiveClub(roster, &team) ||
            team == _selTeamNumber)
            continue;
        clubs[count].roster = roster;
        clubs[count].country = World_FileCountry(team >> 8);
        clubs[count].next_domestic = first_in_country[clubs[count].country];
        first_in_country[clubs[count].country] = count + 1;
        clubs[count].quota = (uint8_t)(4 + next_random(&seed) % 3);
        order[count] = count;
        ++count;
    }

    for (size_t i = count; i > 1; --i) {
        size_t j = next_random(&seed) % i, temp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = temp;
    }

    uint32_t run = ++world->ai_transfer_run;
    unsigned domestic = 0, international = 0;
    for (size_t i = 0; i < count; ++i) {
        TransferClub *a = &clubs[order[i]];
        size_t domestic_order[512], domestic_count = 0;
        for (size_t j = first_in_country[a->country]; j; j = clubs[j - 1].next_domestic) {
            if (&clubs[j - 1] != a && domestic_count < sizeof domestic_order / sizeof *domestic_order)
                domestic_order[domestic_count++] = j - 1;
        }
        for (unsigned attempt = 0; a->completed < a->quota && attempt < 1100; ++attempt) {
            int abroad = next_random(&seed) % 100 < 15;
            int higher = next_random(&seed) % 100 < 80;
            if (!abroad && !domestic_count) continue;
            size_t partner_index = abroad ? next_random(&seed) % count :
                domestic_order[next_random(&seed) % domestic_count];
            TransferClub *b = &clubs[partner_index];
            if (a == b || b->completed >= b->quota || (a->country != b->country) != abroad)
                continue;
            size_t first = next_random(&seed) % a->roster->count;
            ModernRosterPlayer *player = &a->roster->players[first];
            if (recently_transferred(player, run)) continue;
            unsigned value = transfer_value(player);
            if (!value) continue;
            size_t start = next_random(&seed) % b->roster->count;
            for (size_t j = 0; j < b->roster->count; ++j) {
                size_t second = (start + j) % b->roster->count;
                ModernRosterPlayer *partner = &b->roster->players[second];
                if (recently_transferred(partner, run) ||
                    !transfer_candidate(player, partner, value, higher))
                    continue;
                ModernRosterPlayer saved = *player;
                *player = *partner;
                *partner = saved;
                partner->swos.shirtNumber = player->swos.shirtNumber;
                player->swos.shirtNumber = saved.swos.shirtNumber;
                player->last_ai_transfer = partner->last_ai_transfer = run;
                ++a->completed;
                ++b->completed;
                ModernCareer_ClearPreferred16(a->roster);
                ModernCareer_ClearPreferred16(b->roster);
                if (abroad) ++international; else ++domestic;
                break;
            }
        }
    }
    free(order);
    free(clubs);

    uint16_t parked_count;
    TeamFileEx *parked = Career_ParkedTeams(&parked_count);
    refresh_teams(world, _selectedTeams, _numSelectedTeams);
    refresh_teams(world, (TeamFileEx *)_tmdFileBuffer, _careerFileBuffer);
    refresh_teams(world, parked, parked_count);
    fprintf(stderr, "[modern-ai] %u domestic, %u international exchanges\n", domestic, international);
    if (show_summary) {
        char message[120];
        const char *const buttons[] = {"CONTINUE"};
        snprintf(message, sizeof message, "%u DOMESTIC EXCHANGES|%u INTERNATIONAL EXCHANGES",
                 domestic, international);
        MenuDialog_Show("AI TRANSFER MARKET", message, buttons, 1, 0);
    }
    return domestic + international;
}

void ModernAI_TransfersMenu(void)
{
    uint32_t run = ModernWorld_Active()->ai_transfer_run + 1;
    ModernAI_RunTransfers((uint32_t)time(NULL) ^ (run * UINT32_C(2654435761)), 1);
}
