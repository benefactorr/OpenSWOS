#include "season_switch.h"
#include "modern_career_runtime.h"
#include "team_pool.h"
#include "endian_helpers.h"
#include "world.h"
#include "play_match_menu.h"
#include <stdlib.h>
#include <string.h>

/* Matches original selectedTeams/TMD buffers, with slack. A pointer can be
 * re-used only after its preceding binding is explicitly retired. */
#define MODERN_RUNTIME_BINDINGS 384u
#define MODERN_LEGACY_ROSTER_CAPACITY 32u

typedef struct RuntimeBinding {
    TeamFileEx *address;
    ModernMatchSelection map;
} RuntimeBinding;
static RuntimeBinding bindings[MODERN_RUNTIME_BINDINGS];
static size_t nbindings;
/* The legacy career already owns a 32-player record buffer. Track permanent
 * IDs for those physical slots; navigation affects playerNumbers, not IDs. */
static struct {
    char club_id[MODERN_ID_LEN];
    char slot_ids[MODERN_LEGACY_ROSTER_CAPACITY][MODERN_ID_LEN];
    size_t shown;
    TeamFileEx *team;
} expandedCareer;

static ModernRoster *roster_for(ModernWorld *world, const TeamFileEx *team);
extern TeamFileEx _careerTeam;
extern uint16_t _selTeamNumber;

int ModernRuntime_Requested(void) {
#ifdef CAREER_PLUS_SEASON_SWITCH
    return World_Current() == WORLD_2627;
#else
    const char *v=getenv("SWOS_MODERN_ROSTERS");
    return v && !strcmp(v,"1");
#endif
}
int ModernRuntime_Enabled(void) {
    return ModernRuntime_Requested() && World_Current() == WORLD_2627;
}
void ModernRuntime_ProjectOpponent(ModernWorld *world, TeamFileEx *team)
{
    ModernRoster *roster = roster_for(world, team);
    size_t slots[MODERN_MATCH_PLAYERS];
    if (!roster || team == &_careerTeam || !ModernCareer_SelectAI16(roster, slots)) return;
    for (size_t i = 0; i < MODERN_MATCH_PLAYERS; ++i)
        team->players[i] = roster->players[slots[i]].swos;
    for (size_t i = 0; i < MODERN_MATCH_PLAYERS; ++i)
        team->team.playerNumbers[i] = (uint8_t)i;
    team->team.tactics = 0; /* 4-4-2 */
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
}

void ModernRuntime_RefreshOpponent(ModernWorld *world, TeamFileEx *team, uint16_t managedTeam)
{
    if (!team || team == &_careerTeam ||
        PACK_BE_U16(team->team.countryNumber, team->team.teamOrdinal) == managedTeam ||
        team->team.teamControls != TEAM_COMPUTER ||
        (team->team.countryNumber >= CN_EUROPE && team->team.countryNumber <= CN_OCEANIA))
        return;
    TeamFileEx previous = *team;
    ModernRuntime_ProjectOpponent(world, team);
    /* Projecting after a transfer or an old save can move a player to a new
     * physical slot. Keep match status and goals for surviving players. */
    for (size_t i = 0; i < MODERN_MATCH_PLAYERS; ++i) {
        PlayerFile *player = &team->players[i];
        for (size_t j = 0; j < MODERN_MATCH_PLAYERS; ++j) {
            const PlayerFile *old = &previous.players[j];
            if (old->nationality == player->nationality && old->shirtNumber == player->shirtNumber &&
                !strncmp(old->playerName, player->playerName, sizeof player->playerName)) {
                player->cardsInjuries = old->cardsInjuries;
                memcpy(player->goalStats, old->goalStats, sizeof player->goalStats);
                break;
            }
        }
    }
}

static unsigned starting_eleven_price(const ModernRoster *roster)
{
    size_t slots[MODERN_MATCH_PLAYERS];
    if (!ModernCareer_SelectAI16(roster, slots)) return 0;
    unsigned total = 0;
    for (size_t i = 0; i < 11; ++i) {
        unsigned price = (uint8_t)roster->players[slots[i]].swos.price;
        total += price <= 49 ? price : 0;
    }
    return (total + 5) / 11;
}

uint32_t ModernRuntime_ScaleOpeningBalance(ModernWorld *world, uint16_t team,
                                          unsigned division, uint32_t balance)
{
    ModernRoster *roster = ModernRuntime_Roster(world, team >> 8, team & 255);
    if (!roster) return balance;
    uint8_t country = World_FileCountry(team >> 8);
    unsigned clubPrice = starting_eleven_price(roster);
    unsigned leaguePrice = 0, clubs = 0;
    for (size_t i = 0; i < world->count; ++i) {
        const ModernRoster *peer = &world->clubs[i];
        if (peer->header.league != division) continue;
        uint16_t liveTeam;
        if (!ModernRuntime_ActiveClub(peer, &liveTeam) ||
            World_FileCountry(liveTeam >> 8) != country)
            continue;
        leaguePrice += starting_eleven_price(peer);
        ++clubs;
    }
    leaguePrice = clubs ? (leaguePrice + clubs / 2) / clubs : clubPrice;
    /* Give equal weight to the club's XI and the division it competes in.
     * A combined price band of 24 retains the existing budget. The weakest
     * club still receives 15 percent, with a minimum of one accounting unit. */
    unsigned strength = clubPrice + leaguePrice;
    unsigned percent = strength >= 48 ? 100 : 15 + 85 * strength * strength / (48 * 48);
    uint32_t scaled = (uint32_t)(((uint64_t)balance * percent / 100) / 1000) * 1000;
    return scaled || !balance ? scaled : 1000;
}
void ModernRuntime_Reset(void) {
    memset(bindings,0,sizeof bindings);
    nbindings=0;
    memset(&expandedCareer,0,sizeof expandedCareer);
}
size_t ModernRuntime_BindingCount(void) {return nbindings;}
int ModernRuntime_PlayerIsBound(const char *player_uuid) {
    size_t i,j;
    if(!player_uuid||!player_uuid[0])return 0;
    for(i=0;i<nbindings;i++)
        for(j=0;j<MODERN_MATCH_PLAYERS;j++)
            if(!strcmp(bindings[i].map.player_ids[j],player_uuid))return 1;
    return 0;
}

int ModernRuntime_PlayerUUID(const TeamFileEx *team,unsigned slot,
                             char out[MODERN_ID_LEN]) {
    size_t i;
    if (!team || !out) return 0;
    if (expandedCareer.team == team && slot < expandedCareer.shown) {
        memcpy(out, expandedCareer.slot_ids[slot], MODERN_ID_LEN);
        return out[0] != 0;
    }
    if (slot >= MODERN_MATCH_PLAYERS) return 0;
    for(i=0;i<nbindings;i++)
        if(bindings[i].address==team) {
            memcpy(out,bindings[i].map.player_ids[slot],MODERN_ID_LEN);
            return out[0]!=0;
        }
    return 0;
}
int ModernRuntime_Suspend(ModernWorld *world,TeamFileEx *team) {
    size_t i;
    if(!world||!team)return 0;
    for(i=0;i<nbindings;i++)if(bindings[i].address==team) {
        if(!ModernRuntime_Flush(world,team))return 0;
        if(expandedCareer.team==team)memset(&expandedCareer,0,sizeof expandedCareer);
        if(i+1<nbindings)
            memmove(&bindings[i],&bindings[i+1],
                    (nbindings-i-1)*sizeof bindings[0]);
        memset(&bindings[--nbindings],0,sizeof bindings[0]);
        return 1;
    }
    return 1;
}
ModernRoster *ModernRuntime_Roster(ModernWorld *world, uint8_t file, uint8_t ordinal) {
    uint8_t country = World_FileCountry(file);
    uint16_t slot = World_Slot(file, ordinal);
    const TeamPoolState *pool = TeamPool_State(country);
    if (pool && pool->map[slot]) {
        uint16_t identity = pool->map[slot] - 1;
        if (identity < TEAM_POOL_SLOTS) {
            uint16_t team = World_SlotTeam(country, identity);
            return ModernWorld_FindSwos(world, team >> 8, team & 255);
        }
        int32_t count;
        const TeamPool *definitions = World_TeamPools(&count);
        for (int32_t i = 0; i < count; ++i)
            if (definitions[i].country == country)
                return ModernWorld_FindSwos(world, definitions[i].file, identity - TEAM_POOL_SLOTS);
        return NULL;
    }
    return ModernWorld_FindSwos(world, file, ordinal);
}

/* Reverse the live pool map so promoted clubs keep their own UUID rosters. */
int ModernRuntime_ActiveClub(const ModernRoster *roster, uint16_t *team) {
    uint8_t file = roster->header.countryNumber;
    uint8_t country = World_FileCountry(file);
    uint16_t slot = World_Slot(file, roster->header.teamOrdinal);
    uint16_t identity = slot;
    int32_t count = 0;
    const TeamPool *pools = World_TeamPools(&count);
    for (int32_t i = 0; i < count; ++i) {
        if (pools[i].file == file) {
            country = pools[i].country;
            identity = TEAM_POOL_SLOTS + roster->header.teamOrdinal;
            break;
        }
    }
    const uint8_t *countries = World_SeasonCountries();
    if (!countries) return 0;
    while (*countries != 255 && *countries != country) ++countries;
    if (*countries == 255) return 0;
    const TeamPoolState *pool = TeamPool_State(country);
    if (pool) {
        for (uint16_t i = 0; i < TEAM_POOL_SLOTS; ++i) {
            if (pool->map[i] == identity + 1) {
                *team = World_SlotTeam(country, i);
                return 1;
            }
        }
    }
    if (identity >= TEAM_POOL_SLOTS || (pool && pool->map[slot])) return 0;
    *team = PACK_BE_U16(file, roster->header.teamOrdinal);
    return 1;
}

unsigned ModernRuntime_PlayerAge(ModernWorld *world, const TeamFileEx *team, unsigned slot) {
    const ModernRoster *roster = roster_for(world, team);
    char id[MODERN_ID_LEN];
    if (!roster) return 0;
    if (ModernRuntime_PlayerUUID(team, slot, id)) {
        for (size_t i = 0; i < roster->count; ++i)
            if (!strcmp(roster->players[i].id, id)) return roster->players[i].age;
        return 0;
    }

    /* AI squad views have a read-only projection without a UUID binding. */
    if (slot >= MODERN_MATCH_PLAYERS) return 0;
    const PlayerFile *visible = &team->players[slot];
    if (!visible->playerName[0] || (uint8_t)visible->playerName[0] == 255) return 0;
    const ModernRosterPlayer *match = NULL;
    for (size_t i = 0; i < roster->count; ++i) {
        const ModernRosterPlayer *player = &roster->players[i];
        if (player->swos.nationality != visible->nationality ||
            player->swos.shirtNumber != visible->shirtNumber ||
            strncmp((const char *)player->swos.playerName, (const char *)visible->playerName,
                    sizeof visible->playerName))
            continue;
        if (match) return 0;
        match = player;
    }
    return match ? match->age : 0;
}

static ModernRoster *roster_for(ModernWorld *world, const TeamFileEx *team) {
    if (!team) return NULL;
    return ModernRuntime_Roster(world, team->team.countryNumber, team->team.teamOrdinal);
}
static int same_identity(const ModernMatchSelection *map,const ModernRoster *roster) {
    return map && roster && !strcmp(map->club_id,roster->club_id);
}
/* This is deliberately fail-closed. Never write data from a reused cache slot
 * to a different club or a roster with missing player UUIDs. */
int ModernRuntime_Flush(ModernWorld *world,TeamFileEx *team) {
    size_t i;
    ModernRoster *r;
    if(!world||!team)return 0;
    for(i=0;i<nbindings;i++) if(bindings[i].address==team) {
        r=roster_for(world,team);
        if(!same_identity(&bindings[i].map,r))return 0;
        if (expandedCareer.team == team)
            return ModernRuntime_CommitCareerSquad(world, team);
        return ModernCareer_CommitMatch(r,team,&bindings[i].map);
    }
    return 1; /* unbound team has nothing to commit */
}
int ModernRuntime_FlushAll(ModernWorld *world) {
    size_t i;
    if(!world)return 0;
    /* Validate every binding before any write-back. A caller can retry or
     * abandon on failure without assigning a changed slot to the wrong club. */
    for(i=0;i<nbindings;i++) {
        ModernRoster *r=roster_for(world,bindings[i].address);
        size_t a,b;
        if(!same_identity(&bindings[i].map,r))return 0;
        for(a=0;a<MODERN_MATCH_PLAYERS;a++) {
            int found=0;
            for(b=0;b<r->count;b++)
                if(!strcmp(r->players[b].id,bindings[i].map.player_ids[a])) {found=1;break;}
            if(!found)return 0;
        }
    }
    for(i=0;i<nbindings;i++)
        if(!ModernRuntime_Flush(world,bindings[i].address))return 0;
    return 1;
}
int ModernRuntime_Activate(ModernWorld *world,TeamFileEx *team) {
    size_t i,slots[MODERN_MATCH_PLAYERS];
    ModernRoster *r;
    ModernMatchSelection selection;
    TeamFileEx projected;
    if(!world||!team)return 0;
    r=roster_for(world,team);
    if(!r)return 0;
    for(i=0;i<nbindings;i++)if(bindings[i].address==team) {
        /* Never silently replace a slot that has been reused for another club. */
        return same_identity(&bindings[i].map,r);
    }
    /* Two live legacy buffers for the same club would race on flush and
     * overwrite each other's player changes. Keep only one owner. */
    for(i=0;i<nbindings;i++)
        if(!strcmp(bindings[i].map.club_id,r->club_id))return 0;
    if(nbindings==MODERN_RUNTIME_BINDINGS)return 0;
    /* A former human club retains preferred16 in old saves. It must not
     * dictate the AI's 4-4-2 or displace more valuable reserves. */
    const int managed = team == &_careerTeam ||
        PACK_BE_U16(team->team.countryNumber, team->team.teamOrdinal) == _selTeamNumber;
    if(!(managed ? ModernCareer_Select16(r,slots)
                 : ModernCareer_SelectAI16(r,slots)) ||
       !ModernCareer_BeginMatch(r,slots,&projected,&selection))return 0;
    /* Keep the career's tactics, funds, kit and mutable team metadata. Only
     * replace the 16 match-visible PlayerFile records. */
    memcpy(team->players,projected.players,sizeof team->players);
    for (size_t slot = 0; slot < MODERN_MATCH_PLAYERS; ++slot)
        team->team.playerNumbers[slot] = (uint8_t)slot;
    if (!managed) team->team.tactics = 0; /* standard SWOS 4-4-2 */
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
    bindings[nbindings].address=team;
    bindings[nbindings].map=selection;
    nbindings++;
    return 1;
}

/* R7: Reassign a match-day squad using UUIDs. The previous binding must be
 * committed before replacing the visible slots; no reserve is removed. */
int ModernRuntime_Choose16(ModernWorld *world,TeamFileEx *team,const size_t slots[16]) {
    ModernRoster *r;
    TeamFileEx projected;
    ModernMatchSelection fresh;
    size_t i,owner=MODERN_RUNTIME_BINDINGS;
    if(!world||!team||!slots)return 0;
    r=roster_for(world,team);
    if(!r)return 0;
    /* Never permit two live team buffers to claim the same UUID roster. */
    for(i=0;i<nbindings;i++) {
        if(bindings[i].address==team)owner=i;
        else if(!strcmp(bindings[i].map.club_id,r->club_id))return 0;
    }
    if(owner==MODERN_RUNTIME_BINDINGS && nbindings>=MODERN_RUNTIME_BINDINGS)return 0;
    if(owner!=MODERN_RUNTIME_BINDINGS &&
       (!same_identity(&bindings[owner].map,r)||
        !ModernRuntime_Flush(world,team)))return 0;
    /* Validation and projection happen before any permanent change. */
    if(!ModernCareer_BeginMatch(r,slots,&projected,&fresh))return 0;
    if(!ModernCareer_SetPreferred16(r,slots))return 0;
    if(owner==MODERN_RUNTIME_BINDINGS)owner=nbindings++;
    memcpy(team->players,projected.players,sizeof team->players);
    for (size_t slot = 0; slot < MODERN_MATCH_PLAYERS; ++slot)
        team->team.playerNumbers[slot] = (uint8_t)slot;
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
    bindings[owner].address=team;
    bindings[owner].map=fresh;
    return 1;
}

/* SWOS originally supports up to 32 players in human career mode: the first
 * 16 standard TEAM records, plus 16 extended records. This operation exposes
 * the modern reserves through the ORIGINAL squad menu and its scroll arrow.
 * No replacement SDL screen and no modification to the match-team format. */
extern uint16_t _careerNumPlayers;
extern uint8_t _currentMatchPlayers[32];
static PlayerFile *extended_player(TeamFileEx *team,size_t slot) {
    return (PlayerFile *)((uint8_t *)team+sizeof(TeamFile)+slot*sizeof(PlayerFile));
}
int ModernRuntime_ExpandCareerSquad(ModernWorld *world, TeamFileEx *team) {
    ModernRoster *r;
    size_t i,j,shown,used=0,bindingIndex;
    if(!world||!team)return 0;
    r=roster_for(world,team);
    if(!r||r->count<MODERN_MATCH_PLAYERS)return 0;
    if(expandedCareer.team==team &&
       !strcmp(expandedCareer.club_id,r->club_id) &&
       _careerNumPlayers==expandedCareer.shown)return 1;
    if(expandedCareer.team &&
       strcmp(expandedCareer.club_id,r->club_id))return 0;
    if(!ModernRuntime_Activate(world,team))return 0;
    for(bindingIndex=0;bindingIndex<nbindings;bindingIndex++)
        if(bindings[bindingIndex].address==team)break;
    if(bindingIndex==nbindings ||
       strcmp(bindings[bindingIndex].map.club_id,r->club_id))return 0;
    shown=r->count<MODERN_LEGACY_ROSTER_CAPACITY?
          r->count:MODERN_LEGACY_ROSTER_CAPACITY;
    memset(&expandedCareer,0,sizeof expandedCareer);
    expandedCareer.team=team;
    memcpy(expandedCareer.club_id,r->club_id,MODERN_ID_LEN);
    expandedCareer.shown=shown;
    for(i=0;i<MODERN_MATCH_PLAYERS;i++) {
        memcpy(expandedCareer.slot_ids[i],
               bindings[bindingIndex].map.player_ids[i],MODERN_ID_LEN);
        for(j=0;j<r->count;j++)
            if(!strcmp(r->players[j].id,expandedCareer.slot_ids[i]))break;
        if(j==r->count)return 0;
    }
    for(i=MODERN_MATCH_PLAYERS;i<shown;i++) {
        while(used<r->count) {
            int selected=0;
            for(j=0;j<MODERN_MATCH_PLAYERS;j++)
                if(!strcmp(r->players[used].id,expandedCareer.slot_ids[j])) {
                    selected=1;break;
                }
            if(!selected)break;
            used++;
        }
        if(used==r->count)return 0;
        memcpy(extended_player(team,i),&r->players[used].swos,sizeof(PlayerFile));
        memcpy(expandedCareer.slot_ids[i],r->players[used].id,MODERN_ID_LEN);
        used++;
    }
    for(i=0;i<shown;i++)_currentMatchPlayers[i]=(uint8_t)i;
    _careerNumPlayers=(uint16_t)shown;
    return 1;
}
int ModernRuntime_RebindCareerSquad(ModernWorld *world, TeamFileEx *team,
                                    const char previous[32][MODERN_ID_LEN],
                                    const uint8_t order[32], size_t previous_count)
{
    ModernRoster *roster = roster_for(world, team);
    if (!roster || roster->count < MODERN_MATCH_PLAYERS ||
        roster->count > MODERN_LEGACY_ROSTER_CAPACITY ||
        previous_count > MODERN_LEGACY_ROSTER_CAPACITY ||
        expandedCareer.team || nbindings >= MODERN_RUNTIME_BINDINGS) return 0;
    for (size_t i = 0; i < nbindings; ++i)
        if (bindings[i].address == team || same_identity(&bindings[i].map, roster)) return 0;

    size_t count = roster->count;
    int physical[32], old_to_new[32];
    uint8_t used[32] = {0}, selected[32] = {0};
    uint8_t next_order[32];
    memset(physical, -1, sizeof physical);
    memset(old_to_new, -1, sizeof old_to_new);
    memset(next_order, UINT8_MAX, sizeof next_order);

    /* Keep the original physical slots where possible. Pack surviving high
     * slots only when a departure makes the live squad smaller. */
    for (size_t old = 0; old < previous_count; ++old) {
        for (size_t player = 0; player < count; ++player) {
            if (strcmp(previous[old], roster->players[player].id)) continue;
            size_t slot = old;
            if (slot >= count) {
                for (slot = 0; physical[slot] >= 0; ++slot) {}
            }
            physical[slot] = (int)player;
            old_to_new[old] = (int)slot;
            used[player] = 1;
            break;
        }
    }
    for (size_t slot = 0, player = 0; slot < count; ++slot) {
        if (physical[slot] >= 0) continue;
        while (used[player]) ++player;
        physical[slot] = (int)player;
        used[player++] = 1;
    }

    /* Reserve every surviving starter and substitute before filling gaps.
     * When someone leaves the matchday squad, prefer a new signing over
     * promoting an existing reserve. Only the vacant matchday slot changes. */
    for (size_t position = 0; position < MODERN_MATCH_PLAYERS && position < previous_count; ++position) {
        unsigned old = order[position];
        if (old >= previous_count || old_to_new[old] < 0) continue;
        unsigned slot = (unsigned)old_to_new[old];
        next_order[position] = (uint8_t)slot;
        selected[slot] = 1;
    }
    uint8_t candidates[32];
    size_t candidate_count = 0;
    for (size_t slot = 0; slot < count; ++slot) {
        size_t old = 0;
        while (old < previous_count && old_to_new[old] != (int)slot) ++old;
        if (old == previous_count) candidates[candidate_count++] = (uint8_t)slot;
    }
    size_t newcomers = candidate_count;
    for (size_t position = MODERN_MATCH_PLAYERS; position < previous_count; ++position) {
        unsigned old = order[position];
        if (old < previous_count && old_to_new[old] >= 0)
            candidates[candidate_count++] = (uint8_t)old_to_new[old];
    }
    for (size_t position = 0; position < MODERN_MATCH_PLAYERS; ++position) {
        if (next_order[position] != UINT8_MAX) continue;
        for (size_t i = 0; i < candidate_count; ++i) {
            unsigned slot = candidates[i];
            if (selected[slot] || (position == 0 &&
                (roster->players[physical[slot]].swos.positionAndFace & PF_POSITION_MASK))) continue;
            next_order[position] = (uint8_t)slot;
            selected[slot] = 1;
            break;
        }
        if (next_order[position] == UINT8_MAX) {
            /* A remaining goalkeeper may already be selected on the bench. */
            for (size_t i = 1; position == 0 && i < MODERN_MATCH_PLAYERS; ++i) {
                unsigned slot = next_order[i];
                if (slot != UINT8_MAX &&
                    !(roster->players[physical[slot]].swos.positionAndFace & PF_POSITION_MASK)) {
                    next_order[0] = (uint8_t)slot;
                    next_order[i] = UINT8_MAX;
                    break;
                }
            }
            if (next_order[position] == UINT8_MAX) return 0;
        }
    }
    size_t position = MODERN_MATCH_PLAYERS;
    for (size_t i = newcomers; i < candidate_count; ++i)
        if (!selected[candidates[i]]) next_order[position++] = candidates[i];
    for (size_t i = 0; i < newcomers; ++i)
        if (!selected[candidates[i]]) next_order[position++] = candidates[i];
    if (position != count) return 0;

    RuntimeBinding *binding = &bindings[nbindings++];
    binding->address = team;
    memcpy(binding->map.club_id, roster->club_id, MODERN_ID_LEN);
    expandedCareer.team = team;
    expandedCareer.shown = count;
    memcpy(expandedCareer.club_id, roster->club_id, MODERN_ID_LEN);
    for (size_t slot = 0; slot < count; ++slot) {
        const ModernRosterPlayer *player = &roster->players[physical[slot]];
        *extended_player(team, slot) = player->swos;
        memcpy(expandedCareer.slot_ids[slot], player->id, MODERN_ID_LEN);
        if (slot < MODERN_MATCH_PLAYERS)
            memcpy(binding->map.player_ids[slot], player->id, MODERN_ID_LEN);
    }
    memcpy(_currentMatchPlayers, next_order, count);
    _careerNumPlayers = (uint16_t)count;
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
    return ModernRuntime_CommitCareerSquad(world, team);
}

int ModernRuntime_AppendCareerPlayer(ModernWorld *world, TeamFileEx *team,
                                    const ModernRosterPlayer *player)
{
    if (!ModernRuntime_ExpandCareerSquad(world, team)) return -1;
    ModernRoster *roster = roster_for(world, team);
    size_t slot = expandedCareer.shown;
    if (slot >= MODERN_LEGACY_ROSTER_CAPACITY || roster->count != slot ||
        !ModernRoster_Add(roster, player)) return -1;

    *extended_player(team, slot) = player->swos;
    memcpy(expandedCareer.slot_ids[slot], player->id, MODERN_ID_LEN);
    _currentMatchPlayers[slot] = (uint8_t)slot;
    expandedCareer.shown++;
    _careerNumPlayers++;
    team->team.playerValuesDirty = TEAM_VALUES_DIRTY;
    return (int)slot;
}

int ModernRuntime_CommitCareerSquad(ModernWorld *world, TeamFileEx *team) {
    ModernRoster *r;
    size_t i,j,preferred[MODERN_MATCH_PLAYERS],physical[MODERN_LEGACY_ROSTER_CAPACITY];
    if(!world||!team||expandedCareer.team!=team)return 0;
    r=roster_for(world,team);
    if(!r||strcmp(r->club_id,expandedCareer.club_id)||
       _careerNumPlayers!=expandedCareer.shown)return 0;
    /* Preflight all physical slots and selection indices before any write. */
    for(i=0;i<expandedCareer.shown;i++) {
        for(j=0;j<r->count;j++)
            if(!strcmp(r->players[j].id,expandedCareer.slot_ids[i]))break;
        if(j==r->count)return 0;
        physical[i]=j;
    }
    for(i=0;i<MODERN_MATCH_PLAYERS;i++) {
        unsigned slot=_currentMatchPlayers[i];
        if(slot>=expandedCareer.shown)return 0;
        preferred[i]=physical[slot];
        for(j=0;j<i;j++)if(preferred[i]==preferred[j])return 0;
    }
    if(!ModernCareer_SetPreferred16(r,preferred)) ModernCareer_ClearPreferred16(r);
    for(i=0;i<expandedCareer.shown;i++)
        memcpy(&r->players[physical[i]].swos,extended_player(team,i),sizeof(PlayerFile));
    return 1;
}

