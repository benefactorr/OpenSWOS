#include "modern_season.h"
#include "modern_career_roster.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char YouthName[sizeof(((PlayerFile *)0)->playerName)];
static struct {
    YouthName *names;
    size_t count, capacity;
} youth_names[256];

static int valid_youth_name(const char *name)
{
    return name[0] && strncmp(name, "YOUTH ", 6) && strncmp(name, "RES ", 4) &&
           strcspn(name, "0123456789") == strlen(name);
}

void ModernSeason_LoadYouthNames(const char *path,
                                 const NationalTeamRecord *(*national_team)(uint16_t))
{
    for (size_t i = 0; i < 256; ++i)
        youth_names[i].count = 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return;

    char line[160];
    while (fgets(line, sizeof line, file)) {
        unsigned id;
        char name[64];
        if (sscanf(line, "%x,%63[^\r\n]", &id, name) != 2 || id > UINT16_MAX ||
            strlen(name) >= sizeof(YouthName) || !valid_youth_name(name))
            continue;
        const NationalTeamRecord *record = national_team((uint16_t)id);
        if (!record)
            continue;
        unsigned nat = record->nationalityCode;
        if (youth_names[nat].count == youth_names[nat].capacity) {
            size_t capacity = youth_names[nat].capacity ? 2 * youth_names[nat].capacity : 32;
            YouthName *names = realloc(youth_names[nat].names, capacity * sizeof *names);
            if (!names)
                continue;
            youth_names[nat].names = names;
            youth_names[nat].capacity = capacity;
        }
        strcpy(youth_names[nat].names[youth_names[nat].count++], name);
    }
    fclose(file);
}

static uint32_t mix(uint32_t x) {
    x^=x>>16; x*=UINT32_C(0x7feb352d); x^=x>>15;
    x*=UINT32_C(0x846ca68b); return x^(x>>16);
}
static unsigned retirement_chance(unsigned age) {
    static const unsigned rates[] = {10, 20, 40, 80, 90};
    if (age < 35) return 0;
    return rates[age < 39 ? age - 35 : 4];
}

int ModernSeason_PickYouthName(PlayerFile *player, const PlayerFile *first,
                              size_t count, size_t stride, const char *excluded,
                              uint32_t seed)
{
    unsigned nat = (uint8_t)player->nationality;
    size_t names = youth_names[nat].count;
    for (size_t attempt = 0; attempt < names; ++attempt) {
        const char *candidate = youth_names[nat].names[(seed % names + attempt) % names];
        if (excluded && !strncmp(candidate, excluded, sizeof player->playerName))
            continue;
        size_t i = 0;
        for (; i < count; ++i) {
            const PlayerFile *peer = (const PlayerFile *)((const uint8_t *)first + i * stride);
            if (peer != player && !strncmp((const char *)peer->playerName, candidate,
                                           sizeof peer->playerName))
                break;
        }
        if (i == count) {
            memset(player->playerName, 0, sizeof player->playerName);
            strcpy((char *)player->playerName, candidate);
            return 1;
        }
    }
    return 0;
}

static void name_retirement_replacement(const ModernWorld *world, const ModernRoster *club,
                                        size_t slot, const PlayerFile *retired, uint32_t seed)
{
    PlayerFile *player = &club->players[slot].swos;
    const char *old_name = (const char *)retired->playerName;
    char *name = (char *)player->playerName;
    if (ModernSeason_PickYouthName(player, &club->players[0].swos, club->count,
                                  sizeof *club->players, old_name, seed))
        return;

    ModernCareer_NameReplacement(world, player, retired, seed);
    if (valid_youth_name(name) && strcmp(name, old_name))
        return;

    static const char *const given[] = {"ADAM", "DANIEL", "LEO", "LUKA", "MATEO", "NOAH", "RAFAEL", "TOMAS"};
    static const char *const surnames[] = {"COSTA", "DANIELS", "MARTIN", "MORENO", "NOVAK", "SILVA", "SOTO", "TORRES"};
    unsigned index = seed % 64;
    snprintf(name, sizeof player->playerName, "%s %s", given[index / 8], surnames[index % 8]);
    if (!strcmp(name, old_name))
        snprintf(name, sizeof player->playerName, "%s %s", given[(index / 8 + 1) % 8], surnames[index % 8]);
}

static void replace_retired(ModernWorld *world, ModernRoster *club, size_t slot, unsigned season) {
    ModernRosterPlayer *player = &club->players[slot];
    unsigned team = (club->header.countryNumber << 8) | club->header.teamOrdinal;
    unsigned appearance = player->swos.positionAndFace & (PF_POSITION_MASK | PF_FACE_MASK);
    unsigned nationality = player->swos.nationality;
    unsigned shirt = player->swos.shirtNumber;
    PlayerFile retired = player->swos;
    size_t selected = MODERN_MATCH_PLAYERS;
    if (club->has_preferred16)
        for (size_t i = 0; i < MODERN_MATCH_PLAYERS; ++i)
            if (!strcmp(club->preferred16[i], player->id)) { selected = i; break; }
    memset(&player->swos, 0, sizeof player->swos);
    player->swos.nationality = (uint8_t)nationality;
    snprintf(player->id, sizeof player->id, "53573237-%04x-8000-8000-%08x%04x",
             team, season, (unsigned)slot);
    if (selected < MODERN_MATCH_PLAYERS)
        memcpy(club->preferred16[selected], player->id, MODERN_ID_LEN);
    uint32_t seed = mix(season ^ (team << 16) ^ (unsigned)slot);
    name_retirement_replacement(world, club, slot, &retired, seed);
    player->swos.shirtNumber = (uint8_t)shirt;
    player->swos.positionAndFace = (uint8_t)appearance;
    ModernCareer_ReplacementSkills(club, &player->swos, &retired, seed);
    player->age = 17;
    player->last_ai_transfer = 0;
    player->is_reserve = 1;
}
int ModernSeason_AdvanceWithRetirements(ModernWorld *w, unsigned season, ModernSeasonStats *out,
                                       ModernRetirementObserver observe, void *context) {
    ModernSeasonStats st={0}; size_t i,k;
    if(!w||!w->clubs||!w->count||!ModernCareer_ValidateWorld(w))return 0;
    st.clubs=w->count;
    /* Age EVERY player, including inactive AI reserves, exactly once per
       call. Caller invokes this only after a completed season transition. */
    for(i=0;i<w->count;i++) {
        ModernRoster *r=&w->clubs[i];
        for(k=0;k<r->count;k++) {
            /* The live roster is projected back into team buffers after
             * InitTeams clears them. Reset the source, including reserves. */
            r->players[k].swos.cardsInjuries = 0;
            if(r->players[k].age<60)r->players[k].age++;
            st.aged++;st.players++;
            uint32_t seed = mix((uint32_t)(i * UINT32_C(65537) + k) ^ mix(season));
            if (seed % 100 < retirement_chance(r->players[k].age)) {
                ModernRosterPlayer retired = r->players[k];
                replace_retired(w, r, k, season);
                if (observe)
                    observe(r, &retired, &r->players[k], context);
                st.retired++;
            }
        }
        size_t preferred[MODERN_MATCH_PLAYERS];
        if (r->has_preferred16 && !ModernCareer_Preferred16(r, preferred))
            ModernCareer_ClearPreferred16(r);
    }
    if(!ModernCareer_ValidateWorld(w))return 0;
    if(out)*out=st;
    return 1;
}


int ModernSeason_Advance(ModernWorld *world, unsigned season, ModernSeasonStats *stats)
{
    return ModernSeason_AdvanceWithRetirements(world, season, stats, NULL, NULL);
}
