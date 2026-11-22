#include "modern_season.h"
#include "modern_career_roster.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t mix(uint32_t x) {
    x^=x>>16; x*=UINT32_C(0x7feb352d); x^=x>>15;
    x*=UINT32_C(0x846ca68b); return x^(x>>16);
}
static unsigned retirement_chance(unsigned age) {
    static const unsigned rates[] = {10, 20, 40, 80, 90};
    if (age < 35) return 0;
    return rates[age < 39 ? age - 35 : 4];
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
    snprintf(player->id, sizeof player->id, "53573237-%04x-8000-8000-%08x%04x",
             team, season, (unsigned)slot);
    if (selected < MODERN_MATCH_PLAYERS)
        memcpy(club->preferred16[selected], player->id, MODERN_ID_LEN);
    uint32_t seed = mix(season ^ (team << 16) ^ (unsigned)slot);
    ModernCareer_NameReplacement(world, &player->swos, &retired, seed);
    if (!strcmp((const char *)player->swos.playerName, (const char *)retired.playerName))
        snprintf((char *)player->swos.playerName, sizeof player->swos.playerName,
                 "YOUTH %04X%02X%04X", team, season, (unsigned)slot);
    player->swos.nationality = (uint8_t)nationality;
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
