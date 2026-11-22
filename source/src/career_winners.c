#include "career_winners.h"

#ifdef SWOS_SDL_BUILD
#include "career_params.h"
#include "comp_identity.h"
#include "menu_dialog.h"
#include <stdio.h>
#include <string.h>

enum { WINNERS_PER_SEASON = 512, PAGE_ROWS = 10 };
typedef struct {
    uint8_t competition, is_league, division;
    uint16_t team;
} Winner;

static Winner history[CAREER_MAX_SEASONS][WINNERS_PER_SEASON];
static uint16_t counts[CAREER_MAX_SEASONS];

void CareerWinners_Clear(void)
{
    memset(counts, 0, sizeof counts);
}

static uint8_t history_id(uint8_t id)
{
    if (id == COMPID_worldCup)
        return COMPID_worldCup2;
    if (id == COMPID_europeanChampionships)
        return COMPID_europeanChampionships2;
    if (id == 120 || id == 121)
        return (uint8_t)(id - 2); /* English cups' working records */
    return id;
}

void CareerWinners_Record(const ModCompetitionResult *result)
{
    if (result->season >= CAREER_MAX_SEASONS || result->competitionId == UINT8_MAX ||
        result->winner == UINT16_MAX)
        return;

    unsigned season = result->season;
    Winner winner = {history_id(result->competitionId), result->kind == MODCOMP_LEAGUE,
                     result->kind == MODCOMP_LEAGUE ? result->division : 0, result->winner};
    for (unsigned i = 0; i < counts[season]; ++i) {
        Winner *entry = &history[season][i];
        if (entry->competition == winner.competition && entry->is_league == winner.is_league &&
            entry->division == winner.division) {
            entry->team = winner.team;
            return;
        }
    }
    if (counts[season] < WINNERS_PER_SEASON)
        history[season][counts[season]++] = winner;
}

void CareerWinners_Save(mjson_print_fn_t output, void *context)
{
    mjson_printf(output, context, ",\n  \"career_winners\":[");
    const char *separator = "";
    for (unsigned season = 0; season < CAREER_MAX_SEASONS; ++season) {
        for (unsigned i = 0; i < counts[season]; ++i) {
            const Winner *entry = &history[season][i];
            mjson_printf(output, context, "%s[%u,%u,%u,%u,%u]", separator, season,
                         (unsigned)entry->competition, entry->is_league ? 0u : 1u,
                         (unsigned)entry->division, (unsigned)entry->team);
            separator = ",";
        }
    }
    mjson_printf(output, context, "]");
}

void CareerWinners_LoadRow(const char *row, int length)
{
    static const char *const paths[] = {"$[0]", "$[1]", "$[2]", "$[3]", "$[4]"};
    static const long limits[] = {CAREER_MAX_SEASONS - 1, 254, MODCOMP_CONTINENTAL, 255, 65534};
    long field[5];
    for (unsigned i = 0; i < 5; ++i)
        if (!mjson_get_long(row, length, paths[i], &field[i]) ||
            field[i] < 0 || field[i] > limits[i])
            return;
    const ModCompetitionResult result = {field[0], field[1], field[2], field[3], field[4]};
    CareerWinners_Record(&result);
}

void CareerWinners_Open(void)
{
    uint8_t id, division;
    int32_t league;
    if (ModApi_ViewedCompetition(&id, &league, &division) != 0)
        return;
    id = history_id(id);

    const Winner *rows[CAREER_MAX_SEASONS];
    unsigned years[CAREER_MAX_SEASONS], total = 0;
    for (int season = CAREER_MAX_SEASONS - 1; season >= 0; --season) {
        for (unsigned i = 0; i < counts[season]; ++i) {
            const Winner *entry = &history[season][i];
            if (entry->competition == id && entry->is_league == !!league &&
                entry->division == (league ? division : 0)) {
                years[total] = (unsigned)season;
                rows[total++] = entry;
                break;
            }
        }
    }
    if (!total) {
        static const char *const done[] = {"BACK"};
        MenuDialog_ShowWithBottomRow("PAST WINNERS", "NO RECORDED WINNERS YET", done, 1, 0);
        return;
    }

    unsigned page = 0, pages = (total + PAGE_ROWS - 1) / PAGE_ROWS;
    for (;;) {
        char text[512];
        size_t used = (size_t)snprintf(text, sizeof text, "PAGE %u OF %u||", page + 1, pages);
        for (unsigned i = page * PAGE_ROWS; i < total && i < (page + 1) * PAGE_ROWS; ++i) {
            char club[18];
            if (ModApi_CareerTeamName(rows[i]->team, club, sizeof club) != 0)
                snprintf(club, sizeof club, "TEAM %04X", (unsigned)rows[i]->team);
            used += (size_t)snprintf(text + used, sizeof text - used, "SEASON %02u   %s|",
                                     years[i] + 1, club);
        }

        const char *buttons[3];
        int previous = -2, next = -2, count = 0;
        if (page)
            buttons[previous = count++] = "PREVIOUS";
        if (page + 1 < pages)
            buttons[next = count++] = "NEXT";
        buttons[count++] = "BACK";
        const int chosen = MenuDialog_ShowWithBottomRow("PAST WINNERS", text, buttons, count, 0);
        if (chosen == previous)
            --page;
        else if (chosen == next)
            ++page;
        else
            break;
    }
}
#endif
