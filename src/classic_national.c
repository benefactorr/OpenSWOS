#ifdef SWOS_SDL_BUILD
#include "game_types.h"
#include "endian_helpers.h"
#include "swos_io.h"
#include "rnc.h"
#include "world.h"
#include "career_management_eligibility.h"
#include "swos_national_trace.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t _nationalTeams[];
extern uint16_t _numNationalTeams, _currentSeasonShowing, _selTeamNumber;
extern uint16_t _gameType, _isNationalSelector, _numForeignMarketPlayers2;
extern MarketPlayer _careerForeignMarketPlayers[];
extern TeamFileEx _careerTeam;
/* Career+ V3: AI-controlled national squads are ephemeral.  A pool of the
 * current club players is built once per season, then each 16-player national
 * TEAM record is assembled from it whenever requested.  The 20-player human
 * international roster is deliberately not changed. */
#define V3_COUNTRY_TEAMS 93u
#define V3_POOL_LIMIT 26000u
#define V3_REPLACEMENT_HASH 131072u

typedef struct V3ClubCandidate {
    PlayerFile player;
    uint16_t club;
    uint8_t slot;
    uint8_t rating;
} V3ClubCandidate;
static V3ClubCandidate v3_club_pool[V3_POOL_LIMIT];
static unsigned v3_pool_count;
static uint8_t v3_pool_country_loaded[80];
static uint16_t v3_pool_season=0xFFFFu;
static uint8_t v3_file_raw[2u + V3_COUNTRY_TEAMS * sizeof(TeamFileEx)];
static uint8_t v3_file_packed[sizeof(v3_file_raw)];
static uint32_t v3_repl_keys[V3_REPLACEMENT_HASH];
static uint16_t v3_repl_indices[V3_REPLACEMENT_HASH];
static uint8_t v3_repl_used[V3_REPLACEMENT_HASH];
static uint8_t v3_building_pool;
typedef struct V3SquadStamp {
    TeamFileEx *address;
    uint16_t team;
    uint16_t season;
} V3SquadStamp;
static V3SquadStamp v3_squad_stamps[128];

extern uint8_t _avgTeamValues[];
extern uint16_t _currentSeasonShowing;
extern uint16_t _isNationalSelector;
extern uint16_t _numForeignMarketPlayers2;
extern MarketPlayer _careerForeignMarketPlayers[];
extern uint8_t _nationalTeams[];
extern uint16_t _numNationalTeams;
extern int DataJson_LoadTeamFileForCountry(int country, unsigned char *dest, int maxTeams);
extern void RncDecompressV1(uint8_t *src,uint8_t *dst);
extern void RncDecompressV2(uint8_t *src,uint8_t *dst);

static uint8_t v3_player_rating(const PlayerFile *p)
{
    /* Compare players with the same rules irrespective of their country.
       The attributes are SWOS nibbles, historically from 0 to 7. */
    unsigned x=(uint8_t)p->passing & 15u;
    unsigned a=(uint8_t)p->velocityHeading;
    unsigned b=(uint8_t)p->tacklingControl;
    unsigned c=(uint8_t)p->speedFinishing;
    return (uint8_t)(x + (a>>4) + (a&15u) + (b>>4) + (b&15u) + (c>>4) + (c&15u));
}
static uint32_t v3_slot_key(uint16_t id,unsigned slot)
{ return ((uint32_t)id<<4)|(slot&15u); }
static unsigned v3_slot_hash(uint32_t key)
{ return (unsigned)((key * 2654435761u) & (V3_REPLACEMENT_HASH-1u)); }
static void v3_index_market(void)
{
    memset(v3_repl_used,0,sizeof(v3_repl_used));
    unsigned n=_numForeignMarketPlayers2;
    if(n>65535u)n=65535u;
    for(unsigned i=0;i<n;i++) {
        const MarketPlayer *p=&_careerForeignMarketPlayers[i];
        if(p->playerOrdinal<0 || p->playerOrdinal>=16)continue;
        const uint8_t *id=(const uint8_t *)&p->teamId;
        uint32_t key=v3_slot_key(PACK_BE_U16(id[0],id[1]),(unsigned)p->playerOrdinal);
        unsigned h=v3_slot_hash(key);
        while(v3_repl_used[h] && v3_repl_keys[h]!=key)
            h=(h+1u)&(V3_REPLACEMENT_HASH-1u);
        v3_repl_used[h]=1;v3_repl_keys[h]=key;v3_repl_indices[h]=(uint16_t)i;
    }
}
static const PlayerFile *v3_market_player(uint16_t id,unsigned slot)
{
    unsigned h=v3_slot_hash(v3_slot_key(id,slot));
    while(v3_repl_used[h]) {
        if(v3_repl_keys[h]==v3_slot_key(id,slot))
            return &_careerForeignMarketPlayers[v3_repl_indices[h]].player;
        h=(h+1u)&(V3_REPLACEMENT_HASH-1u);
    }
    return NULL;
}
static unsigned v3_load_club_country(unsigned country)
{
    /* Read the edited SWOS TEAM files first for the V3 nationality pool.
       The generic data_json corpus may be stale or omit players in the
       original abroad squads (for example BOS internationals in Spain).
       Always fall back to JSON if an actual TEAM file is absent/invalid. */
    char rel[32],path[1024];
    snprintf(rel,sizeof rel,"data/team.%03u",country);
    const char *resolved=swos_data_path(rel,path,(uint32_t)sizeof path);
    FILE *f=fopen(resolved,"rb");
    if(f) {
        size_t len=fread(v3_file_packed,1,sizeof v3_file_packed,f);
        int extra=fgetc(f),err=ferror(f);
        fclose(f);
        if(!err && extra==EOF && len>=2) {
            int valid=1;
            if(len>=18 && v3_file_packed[0]=='R' && v3_file_packed[1]=='N' &&
               v3_file_packed[2]=='C' &&
               (v3_file_packed[3]==1||v3_file_packed[3]==2)) {
                uint32_t size=BE_READ_U32(v3_file_packed+4);
                uint32_t packed=BE_READ_U32(v3_file_packed+8);
                if(size<2 || size>sizeof v3_file_raw || packed>len-18)valid=0;
                else {
                    if(v3_file_packed[3]==1)RncDecompressV1(v3_file_packed,v3_file_raw);
                    else RncDecompressV2(v3_file_packed,v3_file_raw);
                    len=size;
                }
            } else memcpy(v3_file_raw,v3_file_packed,len);
            if(valid) {
                unsigned n=BE_READ_U16(v3_file_raw);
                if(n<=V3_COUNTRY_TEAMS && len==2u+n*sizeof(TeamFileEx))return n;
            }
        }
    }
    const char *dj=getenv("SWOS_DATA_JSON");
    if(!(dj && dj[0]=='0' && !dj[1])) {
        int n=DataJson_LoadTeamFileForCountry((int)country,v3_file_raw,V3_COUNTRY_TEAMS);
        if(n>0 && (unsigned)n<=V3_COUNTRY_TEAMS)return (unsigned)n;
    }
    return 0;
}
static int v3_valid_club_file(unsigned n)
{
    return n<80u && n!=52 && n!=53 && n!=54 &&
        n!=56 && n!=57 && n!=58 && n!=59 && n!=61 && n!=63 &&
        n!=70 && n!=72 && n!=74;
}
static void v3_refresh_pool(void)
{
    if(v3_building_pool || v3_pool_season==_currentSeasonShowing)return;
    v3_building_pool=1;
    v3_pool_count=0;
    memset(v3_pool_country_loaded,0,sizeof v3_pool_country_loaded);
    v3_index_market();
    for(unsigned country=0;country<80u;country++) {
        if(!v3_valid_club_file(country))continue;
        unsigned count=v3_load_club_country(country);
        if(count)v3_pool_country_loaded[country]=1;
        for(unsigned t=0;t<count;t++) {
            TeamFileEx *team=(TeamFileEx *)(v3_file_raw+2u+t*sizeof(TeamFileEx));
            if(team->team.countryNumber>=80u)continue;
            uint16_t id=PACK_BE_U16(team->team.countryNumber,team->team.teamOrdinal);
            /* National call-up eligibility must use the same club roster that
             * the manager sees. The raw TEAM file and market override pool can
             * lag a live current/selected/TMD roster after a transfer or
             * retirement. Never reinstate an obsolete player from that file. */
            extern TeamFileEx *ClassicCareer_CachedTeam(uint16_t teamId);
            TeamFileEx *cached=ClassicCareer_CachedTeam(id);
            if(cached && PACK_BE_U16(cached->team.countryNumber,
                                     cached->team.teamOrdinal)!=id)
                cached=0;
            for(unsigned slot=0;slot<16u;slot++) {
                const PlayerFile *p=cached ? &cached->players[slot] : &team->players[slot];
                if(!cached) {
                    const PlayerFile *market=v3_market_player(id,slot);
                    if(market)p=market;
                }
                if(!p->playerName[0]||(uint8_t)p->playerName[0]==0xFFu)continue;
                if(v3_pool_count==V3_POOL_LIMIT)break;
                V3ClubCandidate *item=&v3_club_pool[v3_pool_count++];
                item->player=*p;item->club=id;item->slot=(uint8_t)slot;
                item->rating=v3_player_rating(p);
            }
        }
    }
    /* Keep the currently managed club's live roster in preference to the
       static file version, so manual transfers are visible immediately. */
    if(!_isNationalSelector) {
        uint16_t current=PACK_BE_U16(_careerTeam.team.countryNumber,
                                     _careerTeam.team.teamOrdinal);
        if(current==_selTeamNumber)for(unsigned i=0;i<v3_pool_count;i++)
            if(v3_club_pool[i].club==current && v3_club_pool[i].slot<16u) {
                const PlayerFile *p=&_careerTeam.players[v3_club_pool[i].slot];
                v3_club_pool[i].player=*p;
                v3_club_pool[i].rating=v3_player_rating(p);
            }
    }
    v3_pool_season=_currentSeasonShowing;
    v3_building_pool=0;
    /* Diagnose the BOS pool by club. This also tells us whether a player
       such as Kodro has been loaded but lost the positional selection. */
    {
        FILE *bosLog=fopen("swos_bosnia_national_pool.log","a");
        if(bosLog) {
            unsigned domestic=0,abroad=0;
            fprintf(bosLog,"SEASON %u BOS nationality=%u\n",
                (unsigned)_currentSeasonShowing,42u);
            for(unsigned i=0;i<v3_pool_count;i++) {
                const V3ClubCandidate *c=&v3_club_pool[i];
                if((uint8_t)c->player.nationality!=42u)continue;
                char playerName[sizeof(c->player.playerName)+1u];
                unsigned n=0;
                while(n<sizeof(c->player.playerName) && c->player.playerName[n] &&
                      (uint8_t)c->player.playerName[n]!=0xFFu) {
                    playerName[n]=c->player.playerName[n];n++;
                }
                playerName[n]=0;
                unsigned clubCountry=c->club>>8;
                if(clubCountry==CN_BOSNIA)domestic++;else abroad++;
                fprintf(bosLog,"BOS club=%04X country=%u slot=%u role=%u value=%u name=%s\n",
                    (unsigned)c->club,clubCountry,(unsigned)c->slot,
                    (unsigned)(((uint8_t)c->player.positionAndFace>>5)&7u),
                    (unsigned)((uint8_t)c->player.price),playerName);
            }
            fprintf(bosLog,"TOTAL domestic=%u abroad=%u\n",domestic,abroad);
            fclose(bosLog);
        }
    }
    FILE *log=fopen("swos_national_squads.log","a");
    if(log){fprintf(log,"POOL season=%u players=%u\n",
        (unsigned)v3_pool_season,v3_pool_count);fclose(log);}
}
static int v3_nationality_for(uint16_t teamId,uint8_t *nationality)
{
    const NationalTeamRecord *table=(const NationalTeamRecord *)_nationalTeams;
    unsigned limit=_numNationalTeams;
    if(limit>145u)limit=145u;
    for(unsigned i=0;i<limit;i++)
        if(PACK_BE_U16(table[i].countryNumber,table[i].teamOrdinal)==teamId) {
            /* Bosnia's domestic club file is TEAM.009, but original SWOS
               players at home and abroad use nationality 42 (BOS).
               Accept only real BOS 42, not the previous, incorrect 9. */
            *nationality=(teamId==0x500Cu)?42u:table[i].nationalityCode;
            return 1;
        }
    return 0;
}
static int v3_ambiguous_nationality(uint8_t nationality,uint16_t teamId)
{
    const NationalTeamRecord *table=(const NationalTeamRecord *)_nationalTeams;
    unsigned limit=_numNationalTeams;
    if(limit>145u)limit=145u;
    for(unsigned i=0;i<limit;i++) {
        uint16_t other=PACK_BE_U16(table[i].countryNumber,table[i].teamOrdinal);
        if(other!=teamId && table[i].nationalityCode==nationality &&
           other>=0x5000u && other<0x5600u)return 1;
    }
    return 0;
}
/* Stable, local pseudo-random generation: never changes match RNG. */
static uint32_t v3_fake_mix(uint32_t n)
{
    n^=n>>16;n*=0x7feb352du;n^=n>>15;
    n*=0x846ca68bu;n^=n>>16;return n;
}
static unsigned v3_fake_roll(uint16_t teamId,unsigned slot,unsigned salt,unsigned max)
{
    uint32_t key=((uint32_t)teamId<<16)^((uint32_t)_currentSeasonShowing*0x9e3779b9u)
       ^((uint32_t)(slot+1u)*0x85ebca6bu)^((uint32_t)(salt+1u)*0xc2b2ae35u);
    return max?(unsigned)(v3_fake_mix(key)%max):0;
}
static unsigned v3_fake_clamp(int n,unsigned low,unsigned high)
{ return (unsigned)(n<(int)low?(int)low:n>(int)high?(int)high:n); }
/* Resolve the generated names for a squad together. Qualification and cup
 * simulation can load a national file many times in one season. */
#define NATIONAL_NAME_BYTES (sizeof(((PlayerFile *)0)->playerName))
static void v3_fake_names(uint16_t teamId, char names[16][NATIONAL_NAME_BYTES],
                          const char *teamName)
{
    for (unsigned slot = 0; slot < 16; ++slot)
        snprintf(names[slot], NATIONAL_NAME_BYTES, "%.15s %02u", teamName, slot + 1);

    char resolved[1024];
    const char *path = swos_data_path("data/national_fake_names.csv", resolved, sizeof resolved);
    FILE *file = fopen(path, "rb");
    if (!file) return;
    char line[160], name[64];
    unsigned id, total = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%x,%63[^\r\n]", &id, name) == 2 &&
            id == teamId && strlen(name) < NATIONAL_NAME_BYTES)
            ++total;
    }
    if (total) {
        /* Retain the original deterministic start and order, including
         * short lists that wrap within one squad. */
        unsigned start = v3_fake_roll(teamId, 0, 101u, total);
        unsigned index = 0, remaining = 16;
        rewind(file);
        while (remaining && fgets(line, sizeof line, file)) {
            if (sscanf(line, "%x,%63[^\r\n]", &id, name) != 2 ||
                id != teamId || strlen(name) >= NATIONAL_NAME_BYTES)
                continue;
            for (unsigned slot = 0; slot < 16; ++slot) {
                if ((start + slot) % total != index) continue;
                strcpy(names[slot], name);
                --remaining;
            }
            ++index;
        }
    }
    fclose(file);
}
/* Match the non-career display value mapping used by SWOS. */
static unsigned v3_national_display_value(const PlayerFile *p)
{
    unsigned price=(uint8_t)p->price;
    unsigned val=(price<128u)?_avgTeamValues[price]:1u;
    if(val<1u)return 1u;
    return val>80u?80u:val;
}
/* Tactical starting slots (0..10) are demands, not player identities.
 * Retaining tactics is insufficient if we fill each physical player record
 * according to the old squad ordering: a side can end up without any RB.
 * 0 GK, 1 RB, 2 LB, 3 D, 4 RW, 5 LW, 6 M, 7 ATT. */
static void v3_tactical_roles(unsigned tactic, uint8_t roles[11])
{
    /* Exact eleven visible SWOS squad rows, in shirt/formation order.
     * 0 GK, 1 RB, 2 LB, 3 D, 4 RW, 5 LW, 6 M, 7 ATT.
     * Do not infer these from the formation digits: e.g. 3-5-2
     * has three central defenders and RW/M/M/M/LW, whereas 4-4-2
     * has RB/D/D/LB and RW/M/M/LW.
     * These entries are in displayed squad-row order, NOT the pitch
     * order used by _positions352 in player_data.c. In the 3-5-2
     * squad display, the display rows 4,5,6,7 require M,D,RW,M. */
    static const uint8_t tacticRoles[12][11] = {
        /* 4-4-2 */ {0,1,3,3,2,4,6,6,5,7,7},
        /* 5-4-1 display order: visible 5=D, 6=LB, 7=RW,
         * 9=M, 10=LW. SWOS rearranges physical slots 5..7 and 9..10
         * when displaying this tactic (confirmed with San Marino). */
        /* 5-4-1 */ {0,1,3,3,2,4,3,6,5,6,7},
        /* 4-5-1: SWOS _positions451 displays physical slots 9 and 10
         * in reverse order; supply LW to physical row 9 and M to row 10. */
        /* 4-5-1 */ {0,1,3,3,2,4,6,6,5,6,7},
        /* 5-3-2: SWOS displays physical rows 5,6,7 as 7,5,6. */
        /* 5-3-2 */ {0,1,3,3,2,4,3,6,5,7,7},
        /* 3-5-2 */ {0,3,3,6,3,4,6,6,5,7,7},
        /* 4-3-3 */ {0,1,3,3,2,4,6,5,7,7,7},
        /* 4-2-4 */ {0,1,3,3,2,6,6,7,7,7,7},
        /* 3-4-3 */ {0,3,3,3,4,6,6,5,7,7,7},
        /* SWEEP: same displayed position requirements as 4-4-2. */
        /* SWEEP */ {0,1,3,3,2,4,6,6,5,7,7},
        /* 5-2-3 */ {0,1,3,3,3,2,6,6,7,7,7},
        /* ATTACK */ {0,3,3,3,4,6,6,5,7,7,7},
        /* DEFEND */ {0,1,3,3,3,2,4,6,6,5,7}
    };
    memcpy(roles,tacticRoles[tactic<12u?tactic:0u],11u);
}
/* Score the role actually occupied on the tactics pitch, not the physical
 * TEAM file slot. Position fidelity has priority; value is maximised
 * across all eleven positions within those requirements. */
static int v3_role_assignment_score(const PlayerFile *p, unsigned role,
                                    unsigned starting)
{
    unsigned natural=((uint8_t)p->positionAndFace>>5)&7u;
    unsigned value=v3_national_display_value(p);
    if(!starting)return 0;
    int compatibility;
    if(natural==role)compatibility=30000;
    else if(role==0u || natural==0u)compatibility=-30000;
    else if((role>=1u&&role<=3u&&natural>=1u&&natural<=3u) ||
            (role>=4u&&role<=6u&&natural>=4u&&natural<=6u))
        compatibility=10000;
    else compatibility=-10000;
    return compatibility+(int)value*64 + (int)v3_player_rating(p);
}
/* Max-weight assignment of the 16 available physical players to the eleven
 * tactical positions and five reserve rows. The Hungarian algorithm avoids
 * greedy promotions which can steal the only RB for a central-defender slot.
 * The last five rows carry no role bonus and take the remaining players. */
static void v3_assign_national_lineup(uint16_t teamId, TeamFileEx *team)
{
    uint8_t roles[11];
    v3_tactical_roles(team->team.tactics,roles);
    int potentialsRow[17]={0}, potentialsCol[17]={0};
    int chosenRow[17]={0}, predecessor[17]={0};
    for(int i=1;i<=16;i++) {
        chosenRow[0]=i;
        int minCost[17],used[17]={0};
        for(int j=0;j<=16;j++){minCost[j]=0x3fffffff;predecessor[j]=0;}
        int current=0;
        do {
            used[current]=1;
            int row=chosenRow[current],delta=0x3fffffff,next=0;
            for(int col=1;col<=16;col++)if(!used[col]) {
                int score=v3_role_assignment_score(&team->players[col-1],
                              row<=11?roles[row-1]:0u,row<=11);
                int cost=-score-potentialsRow[row]-potentialsCol[col];
                if(cost<minCost[col]){minCost[col]=cost;predecessor[col]=current;}
                if(minCost[col]<delta){delta=minCost[col];next=col;}
            }
            for(int col=0;col<=16;col++) {
                if(used[col]){
                    potentialsRow[chosenRow[col]]+=delta;
                    potentialsCol[col]-=delta;
                } else minCost[col]-=delta;
            }
            current=next;
        } while(chosenRow[current]);
        do {
            int old=predecessor[current];
            chosenRow[current]=chosenRow[old];
            current=old;
        } while(current);
    }
    uint8_t result[16]={0};
    for(int col=1;col<=16;col++)if(chosenRow[col])
        result[chosenRow[col]-1]=(uint8_t)(col-1);
    /* Materialise the lineup in physical player-record order as well.
     * Several SWOS paths rebuild playerNumbers from the physical TEAM file
     * after the national squad is generated. Merely changing playerNumbers
     * therefore loses the assignment (e.g. England's 3-5-2). By putting the
     * chosen starting eleven in records 0..10, both identity playerNumbers
     * and any later reset still produce the same tactical positions.
     * All 16 players are moved together, including their attributes. */
    PlayerFile ordered[16];
    for(unsigned row=0;row<16u;row++) {
        ordered[row]=team->players[result[row]];
        ordered[row].shirtNumber=(int8_t)(row+1u);
    }
    memcpy(team->players,ordered,sizeof ordered);
    for(unsigned row=0;row<16u;row++)
        team->team.playerNumbers[row]=(uint8_t)row;
    FILE *log=fopen("swos_national_squads.log","a");
    if(log) {
        fprintf(log,"LINEUP_ASSIGN season=%u team=%04X tactic=%u",
                (unsigned)_currentSeasonShowing,(unsigned)teamId,
                (unsigned)team->team.tactics);
        for(unsigned row=0;row<11u;row++) {
            PlayerFile *p=&team->players[row];
            fprintf(log," %u:%u/%u/%u",row+1u,(unsigned)roles[row],
                (unsigned)(((uint8_t)p->positionAndFace>>5)&7u),
                v3_national_display_value(p));
        }
        fputc('\n',log);fclose(log);
    }
}
static void v3_build_ai_national_team(uint16_t teamId, TeamFileEx *team, int fresh)
{
    if(!team || _currentSeasonShowing==0xFFFFu ||
       teamId<0x5000u || teamId>=0x5600u ||
       (_isNationalSelector && teamId==_selTeamNumber) || v3_building_pool)return;
    uint8_t nationality;
    if(!v3_nationality_for(teamId,&nationality))return;
    /* Avoid rebuilding a cached playing squad on every GetTeamPointer call.
       File-buffer addresses are reused, so both season and team ID are keys. */
    unsigned stampSlot=((uintptr_t)team>>3)&127u;
    V3SquadStamp *stamp=&v3_squad_stamps[stampSlot];
    if(!fresh && stamp->address==team && stamp->team==teamId &&
       stamp->season==_currentSeasonShowing)return;
    v3_refresh_pool();
    /* A few legacy nations share one nationality byte.  Do not transfer
       an unrelated nation's players into either squad.  Those teams
       receive placeholders until a reliable per-player mapping exists. */
    int ambiguous=v3_ambiguous_nationality(nationality,teamId);
    /* Keep the existing 16-slot formation and tactic. A human national
       team's 20-slot extension is not represented by TeamFileEx. */
    PlayerFile templatePlayers[16];
    char temporaryNames[16][NATIONAL_NAME_BYTES];
    int temporaryNamesReady = 0;
    memcpy(templatePlayers,team->players,sizeof templatePlayers);
    /* Start from the formation's required positions, not whichever natural
       positions happened to occupy the previous first eleven. The physical
       TEAM slots may be in a different order from the visible lineup. */
    uint8_t roleForSlot[16];
    uint8_t tacticalRoles[11];
    v3_tactical_roles(team->team.tactics,tacticalRoles);
    for(unsigned i=0;i<16u;i++)
        roleForSlot[i]=((uint8_t)templatePlayers[i].positionAndFace>>5)&7u;
    uint8_t mapped[16]={0};
    int validMap=1;
    for(unsigned i=0;i<16u;i++) {
        unsigned j=team->team.playerNumbers[i];
        if(j>=16u||mapped[j]){validMap=0;break;}
        mapped[j]=1;
    }
    if(!validMap)for(unsigned i=0;i<16u;i++)
        team->team.playerNumbers[i]=(uint8_t)i;
    for(unsigned i=0;i<11u;i++)
        roleForSlot[team->team.playerNumbers[i]]=tacticalRoles[i];
    unsigned strength[7]={0}, samples=0;
    unsigned avgPrice=0;
    for(unsigned i=0;i<16u;i++) {
        const PlayerFile *p=&templatePlayers[i];
        if((uint8_t)p->playerName[0]==0xFFu||!p->playerName[0])continue;
        unsigned attrs[7]={(uint8_t)p->passing&15u,
           ((uint8_t)p->velocityHeading>>4),((uint8_t)p->velocityHeading&15u),
           ((uint8_t)p->tacklingControl>>4),((uint8_t)p->tacklingControl&15u),
           ((uint8_t)p->speedFinishing>>4),((uint8_t)p->speedFinishing&15u)};
        for(unsigned k=0;k<7;k++)strength[k]+=attrs[k]>7u?7u:attrs[k];
        avgPrice+=(uint8_t)p->price;samples++;
    }
    if(!samples){for(unsigned k=0;k<7;k++)strength[k]=3u;samples=1;avgPrice=8;}
    uint8_t selected[V3_POOL_LIMIT/8+1u];
    memset(selected,0,sizeof selected);
    unsigned real=0,fake=0;
    for(unsigned slot=0;slot<16u;slot++) {
        unsigned role=roleForSlot[slot];
        int best=-1;unsigned bestRating=0;
        for(unsigned i=0;i<v3_pool_count;i++) {
            const V3ClubCandidate *c=&v3_club_pool[i];
            if(ambiguous || (uint8_t)c->player.nationality!=nationality ||
                (((unsigned)c->player.positionAndFace>>5)&7u)!=role ||
                (selected[i>>3]&(1u<<(i&7u))))continue;
            /* Rank primarily by the value visible in SWOS, not by a raw
               sum of skills. That sum can put an expensive forward on the
               bench while a lower-value forward starts. */
            unsigned price=(uint8_t)c->player.price;
            unsigned shown=(price<128u)?_avgTeamValues[price]:1u;
            unsigned score=shown*64u + c->rating;
            if(best<0 || score>bestRating) {best=(int)i;bestRating=score;}
        }
        if(best>=0) {
            team->players[slot]=v3_club_pool[best].player;
            selected[(unsigned)best>>3]|=(uint8_t)(1u<<((unsigned)best&7u));
            real++;
        } else {
            PlayerFile *p=&team->players[slot];
            memset(p,0,sizeof *p);
            p->nationality=(int8_t)nationality;
            p->positionAndFace=(uint8_t)(role<<5);
            if (!temporaryNamesReady) {
                char nationName[sizeof(team->team.teamName) + 1];
                unsigned j = 0;
                while (j < sizeof team->team.teamName && team->team.teamName[j] &&
                       (uint8_t)team->team.teamName[j] != 0xFFu) {
                    nationName[j] = team->team.teamName[j];
                    ++j;
                }
                nationName[j] = 0;
                v3_fake_names(teamId, temporaryNames, nationName);
                temporaryNamesReady = 1;
            }
            strcpy(p->playerName, temporaryNames[slot]);
            unsigned generated[7];
            for(unsigned k=0;k<7;k++) {
                /* Small deviations from original 1996 squad attributes,
                   clamped to the game's actual 0..7 skill range. */
                int delta=(int)v3_fake_roll(teamId,slot,k,5u)-2;
                generated[k]=v3_fake_clamp((int)(strength[k]/samples)+delta,0,7);
            }
            p->passing=(int8_t)generated[0];
            p->velocityHeading=(int8_t)((generated[1]<<4)|generated[2]);
            p->tacklingControl=(int8_t)((generated[3]<<4)|generated[4]);
            p->speedFinishing=(int8_t)((generated[5]<<4)|generated[6]);
            /* Keep prices in the source nation's approximate band. */
            int price=(int)(avgPrice/samples)+(int)v3_fake_roll(teamId,slot,33u,11u)-5;
            p->price=(int8_t)v3_fake_clamp(price,1,100);
            fake++;
        }
        team->players[slot].positionAndFace &= 0xF8u;
        team->players[slot].shirtNumber=templatePlayers[slot].shirtNumber;
        team->players[slot].cardsInjuries=0;
        memset(team->players[slot].goalStats,0,sizeof team->players[slot].goalStats);
        team->players[slot].cachedMatchValue=0;
    }
    /* Optimise the complete eleven against actual tactical slot demands.
       This keeps scarce full-backs/wingers in their roles while maximising
       the displayed value within each available natural position. */
    v3_assign_national_lineup(teamId,team);
    team->team.playerValuesDirty=TEAM_VALUES_DIRTY;
    stamp->address=team;
    stamp->team=teamId;
    stamp->season=_currentSeasonShowing;
    FILE *log=fopen("swos_national_squads.log","a");
    if(log){fprintf(log,"SQUAD season=%u team=%04X tactic=%u real=%u temporary=%u\n",
        (unsigned)_currentSeasonShowing,(unsigned)teamId,
        (unsigned)team->team.tactics,real,fake);fclose(log);}
}


int ClassicNational_ProjectTeam(TeamFileEx *team, int fresh)
{
    if (World_Current() != WORLD_9697 || !team)
        return 0;
    const uint8_t file = team->team.countryNumber;
    if (file < 80 || file > 85)
        return 0;
    v3_build_ai_national_team(PACK_BE_U16(file, team->team.teamOrdinal), team, fresh);
    return 1;
}

int ClassicNational_CanManage(uint16_t teamId)
{
    if (!Career_ManagementTeamAllowed(teamId))
        return 0;
    uint8_t nationality;
    if (!v3_nationality_for(teamId, &nationality))
        return 0;
    v3_refresh_pool();
    unsigned available = 0;
    for (unsigned i = 0; i < v3_pool_count; ++i)
        if ((uint8_t)v3_club_pool[i].player.nationality == nationality && ++available == 16)
            return 1;
    return 0;
}

void ClassicNational_Install(void)
{
    static const uint8_t records[] = {
        0x50, 0x00, 0x00, 0x00, 0x00, 0x13,
        0x50, 0x02, 0x01, 0x01, 0x00, 0x18,
        0x50, 0x04, 0x26, 0x4C, 0x00, 0x12,
        0x50, 0x06, 0x03, 0x03, 0x00, 0x1C,
        0x50, 0x08, 0x05, 0x05, 0x00, 0x10,
        0x50, 0x0C, 0x2A, 0x0A, 0x00, 0x0E,
        0x50, 0x0D, 0x0A, 0x0B, 0x00, 0x0B,
        0x50, 0x0E, 0x0B, 0x0C, 0x00, 0x15,
        0x50, 0x12, 0x0E, 0x0F, 0x00, 0x18,
        0x50, 0x15, 0x10, 0x11, 0x00, 0x11,
        0x50, 0x16, 0x11, 0x13, 0x00, 0x12,
        0x50, 0x18, 0x13, 0x15, 0x00, 0x0E,
        0x50, 0x1A, 0x14, 0x16, 0x00, 0x12,
        0x50, 0x1B, 0x15, 0x17, 0x00, 0x0E,
        0x50, 0x1D, 0x16, 0x18, 0x00, 0x0F,
        0x50, 0x1F, 0x18, 0x1A, 0x00, 0x14,
        0x50, 0x20, 0x19, 0x1B, 0x00, 0x18,
        0x50, 0x23, 0x2E, 0x12, 0x00, 0x1E,
        0x50, 0x26, 0x1E, 0x20, 0x00, 0x0C,
        0x50, 0x28, 0x27, 0x4E, 0x00, 0x17,
        0x50, 0x29, 0x20, 0x22, 0x00, 0x13,
        0x50, 0x2C, 0x2D, 0x25, 0x00, 0x1C,
        0x50, 0x2D, 0x22, 0x26, 0x00, 0x17,
        0x50, 0x2F, 0x24, 0x28, 0x00, 0x1C,
        0x53, 0x03, 0x42, 0x33, 0x00, 0x0B,
        0x53, 0x08, 0x38, 0x3C, 0x00, 0x13,
        0x52, 0x01, 0x45, 0x2D, 0x00, 0x18,
        0x52, 0x03, 0x47, 0x30, 0x00, 0x17,
        0x52, 0x05, 0x49, 0x32, 0x00, 0x11,
        0x52, 0x06, 0x4A, 0x40, 0x00, 0x10,
        0x52, 0x07, 0x4F, 0x41, 0x00, 0x11,
        0x53, 0x0C, 0x4B, 0x42, 0x00, 0x03,
        0x52, 0x09, 0x4D, 0x4D, 0x00, 0x0A,
        0x51, 0x00, 0x50, 0x2A, 0x00, 0x09,
        0x51, 0x0B, 0x58, 0x4F, 0x00, 0x11,
        0x51, 0x19, 0x51, 0x45, 0x00, 0x11,
        0x54, 0x05, 0x78, 0x4B, 0x00, 0x09,
        0x54, 0x09, 0x76, 0x37, 0x00, 0x0C,
        0x54, 0x1B, 0x77, 0x43, 0x00, 0x09,
        0x55, 0x00, 0x94, 0x2C, 0x00, 0x13,
        0x55, 0x02, 0x95, 0x3E, 0x00, 0x0E,
        0x50, 0x05, 0x02, 0x02, 0x00, 0x1E,
        0x50, 0x09, 0x06, 0x06, 0x00, 0x1A,
        0x50, 0x07, 0x04, 0x04, 0x00, 0x1D,
        0x50, 0x0A, 0x07, 0x07, 0x00, 0x1C,
        0x50, 0x0B, 0x08, 0x08, 0x00, 0x22,
        0x50, 0x0F, 0x0C, 0x0D, 0x00, 0x20,
        0x50, 0x11, 0x0D, 0x0E, 0x00, 0x23,
        0x50, 0x13, 0x17, 0x19, 0x00, 0x21,
        0x50, 0x14, 0x0F, 0x10, 0x00, 0x16,
        0x50, 0x17, 0x12, 0x14, 0x00, 0x25,
        0x50, 0x21, 0x1A, 0x1C, 0x00, 0x18,
        0x50, 0x22, 0x1B, 0x1D, 0x00, 0x20,
        0x50, 0x24, 0x1C, 0x1E, 0x00, 0x1F,
        0x50, 0x25, 0x1D, 0x1F, 0x00, 0x1E,
        0x50, 0x27, 0x1F, 0x21, 0x00, 0x1D,
        0x50, 0x2A, 0x28, 0x23, 0x00, 0x21,
        0x50, 0x2B, 0x21, 0x24, 0x00, 0x1E,
        0x50, 0x2E, 0x23, 0x27, 0x00, 0x14,
        0x52, 0x00, 0x44, 0x2B, 0x00, 0x20,
        0x52, 0x02, 0x46, 0x2E, 0x00, 0x24,
        0x52, 0x04, 0x48, 0x31, 0x00, 0x1C,
        0x52, 0x08, 0x4C, 0x47, 0x00, 0x1C,
        0x50, 0x01, 0x29, 0xFF, 0x00, 0x0E,
        0x50, 0x03, 0x2B, 0xFF, 0x00, 0x0E,
        0x50, 0x10, 0x2C, 0xFF, 0x00, 0x13,
        0x50, 0x19, 0x31, 0xFF, 0x00, 0x0B,
        0x50, 0x1C, 0x2F, 0xFF, 0x00, 0x14,
        0x50, 0x1E, 0x32, 0xFF, 0x00, 0x11,
        0x53, 0x00, 0x3D, 0xFF, 0x00, 0x01,
        0x53, 0x01, 0x40, 0xFF, 0x00, 0x10,
        0x53, 0x02, 0x33, 0xFF, 0x00, 0x11,
        0x53, 0x04, 0x35, 0xFF, 0x00, 0x08,
        0x53, 0x05, 0x4E, 0xFF, 0x00, 0x01,
        0x53, 0x06, 0x36, 0xFF, 0x00, 0x03,
        0x53, 0x07, 0x3E, 0xFF, 0x00, 0x01,
        0x53, 0x09, 0x3C, 0xFF, 0x00, 0x01,
        0x53, 0x0A, 0x39, 0xFF, 0x00, 0x09,
        0x53, 0x0B, 0x43, 0xFF, 0x00, 0x01,
        0x53, 0x0C, 0x00, 0x00, 0x00, 0x03,
        0x53, 0x0D, 0x3F, 0xFF, 0x00, 0x14,
        0x51, 0x01, 0x66, 0xFF, 0x00, 0x09,
        0x51, 0x02, 0x6F, 0xFF, 0x00, 0x03,
        0x51, 0x03, 0x52, 0xFF, 0x00, 0x03,
        0x51, 0x04, 0x53, 0xFF, 0x00, 0x01,
        0x51, 0x05, 0x54, 0xFF, 0x00, 0x0B,
        0x51, 0x06, 0x5E, 0xFF, 0x00, 0x11,
        0x51, 0x07, 0x70, 0xFF, 0x00, 0x06,
        0x51, 0x08, 0x69, 0xFF, 0x00, 0x11,
        0x51, 0x09, 0x6C, 0xFF, 0x00, 0x09,
        0x51, 0x0A, 0x6D, 0xFF, 0x00, 0x0E,
        0x51, 0x0C, 0x71, 0xFF, 0x00, 0x0E,
        0x51, 0x0D, 0x5A, 0xFF, 0x00, 0x13,
        0x51, 0x0E, 0x63, 0xFF, 0x00, 0x08,
        0x51, 0x0F, 0x55, 0xFF, 0x00, 0x06,
        0x51, 0x10, 0x61, 0xFF, 0x00, 0x0E,
        0x51, 0x11, 0x75, 0xFF, 0x00, 0x0B,
        0x51, 0x12, 0x5C, 0xFF, 0x00, 0x0E,
        0x51, 0x13, 0x73, 0xFF, 0x00, 0x10,
        0x51, 0x14, 0x62, 0xFF, 0x00, 0x09,
        0x51, 0x15, 0x6B, 0xFF, 0x00, 0x01,
        0x51, 0x16, 0x6B, 0xFF, 0x00, 0x13,
        0x51, 0x17, 0x59, 0xFF, 0x00, 0x17,
        0x51, 0x18, 0x6E, 0xFF, 0x00, 0x06,
        0x51, 0x1A, 0x64, 0xFF, 0x00, 0x0B,
        0x51, 0x1B, 0x65, 0xFF, 0x00, 0x06,
        0x51, 0x1C, 0x6A, 0xFF, 0x00, 0x08,
        0x51, 0x1D, 0x67, 0xFF, 0x00, 0x08,
        0x51, 0x1E, 0x5B, 0xFF, 0x00, 0x01,
        0x51, 0x1F, 0x60, 0xFF, 0x00, 0x07,
        0x51, 0x20, 0x57, 0xFF, 0x00, 0x13,
        0x51, 0x21, 0x68, 0xFF, 0x00, 0x0B,
        0x54, 0x00, 0x3B, 0xFF, 0x00, 0x06,
        0x54, 0x01, 0x79, 0xFF, 0x00, 0x02,
        0x54, 0x02, 0x7A, 0xFF, 0x00, 0x05,
        0x54, 0x03, 0x8B, 0xFF, 0x00, 0x0C,
        0x54, 0x04, 0x36, 0xFF, 0x00, 0x09,
        0x54, 0x06, 0x78, 0x4B, 0x00, 0x08,
        0x54, 0x07, 0x7B, 0xFF, 0x00, 0x0B,
        0x54, 0x08, 0x7B, 0xFF, 0x00, 0x09,
        0x54, 0x0A, 0x7C, 0xFF, 0x00, 0x06,
        0x54, 0x0B, 0x85, 0xFF, 0x00, 0x0E,
        0x54, 0x0C, 0x86, 0xFF, 0x00, 0x01,
        0x54, 0x0D, 0x82, 0xFF, 0x00, 0x06,
        0x54, 0x0E, 0x8D, 0xFF, 0x00, 0x09,
        0x54, 0x0F, 0x8E, 0xFF, 0x00, 0x06,
        0x54, 0x10, 0x87, 0xFF, 0x00, 0x08,
        0x54, 0x11, 0x88, 0xFF, 0x00, 0x06,
        0x54, 0x12, 0x89, 0xFF, 0x00, 0x06,
        0x54, 0x13, 0x8F, 0xFF, 0x00, 0x01,
        0x54, 0x14, 0x8A, 0xFF, 0x00, 0x03,
        0x54, 0x15, 0x92, 0xFF, 0x00, 0x09,
        0x54, 0x16, 0x83, 0xFF, 0x00, 0x0F,
        0x54, 0x17, 0x8C, 0xFF, 0x00, 0x09,
        0x54, 0x18, 0x7F, 0xFF, 0x00, 0x11,
        0x54, 0x19, 0x7D, 0xFF, 0x00, 0x03,
        0x54, 0x1A, 0x7E, 0xFF, 0x00, 0x0B,
        0x54, 0x1C, 0x93, 0xFF, 0x00, 0x0E,
        0x54, 0x1D, 0x81, 0xFF, 0x00, 0x06,
        0x54, 0x1E, 0x84, 0xFF, 0x00, 0x05,
        0x55, 0x01, 0x96, 0xFF, 0x00, 0x01,
        0x55, 0x03, 0x97, 0xFF, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x53, 0x0E, 0x3A, 0x49, 0x00, 0x14,
        0x50, 0x30, 0x25, 0x29, 0x00, 0x1E,
    };
    memcpy(_nationalTeams, records, sizeof records);
    _numNationalTeams = sizeof records / 6;
}

void ClassicNational_Reset(void)
{
    extern uint16_t _teamsLoaded;
    _teamsLoaded = 0;
    v3_pool_season = 0xFFFFu;
    memset(v3_squad_stamps, 0, sizeof v3_squad_stamps);
}
extern uint8_t _careerUnavailablePlayerLookupTable[];
extern void Career_RemoveInternationalPlayersByMask(uint32_t mask);
/* Career+ national roster reconciliation after the annual club retirement
 * and transfer pass. A national PlayerFile is only a copy: its original
 * club/slot can be replaced by a youngster, or the footballer can move.
 * Retired identity checks alone cannot detect a stale national copy when
 * its original lookup now resolves to the replacement's identity.
 *
 * Use the same LIVE club-only pool as dynamic national selection. Match the
 * full stored player name and nationality; do not compare display slots,
 * which are reordered independently from the 20 physical file slots.
 * A unique relocated player gets a corrected source lookup. A player not
 * present at any live club is removed. Ambiguous or incomplete lookups are
 * kept, never guessed, to prevent deleting a legitimate footballer.
 * Existing .CAR structure and identity numbering remain unchanged. */
__attribute__((used, externally_visible))
void SWOS_NationalReconcileAfterSeason(void)
{
    if (!_isNationalSelector || _selTeamNumber < 0x5000u ||
        _selTeamNumber >= 0x5600u) return;

    /* The pool may have been built earlier in the season; the annual AI
     * transfers and retirements changed the live rosters since then. */
    v3_pool_season = 0xFFFFu;
    v3_refresh_pool();
    uint16_t *lookup=(uint16_t *)_careerUnavailablePlayerLookupTable;
    uint32_t removeMask=0;
    unsigned considered=0, kept=0, relinked=0, missing=0, ambiguous=0;
    for (unsigned slot=0;slot<20u;slot++) {
        const PlayerFile *national = (const PlayerFile *)((const uint8_t *)&_careerTeam +
            sizeof(TeamFile) + slot * sizeof(PlayerFile));
        if (!national->playerName[0] ||
            (uint8_t)national->playerName[0]==0xFFu ||
            lookup[slot*2u]==0xFFFFu) continue;
        considered++;
        unsigned matching=0;
        uint16_t matchClub=0;
        uint8_t matchSlot=0;
        /* Prefer exact current club+slot when still occupied by the same
         * footballer. A name appearing in two unrelated clubs is otherwise
         * ambiguous; never relocate on the basis of one of those copies. */
        unsigned currentMatch=0;
        for(unsigned i=0;i<v3_pool_count;i++) {
            const V3ClubCandidate *c=&v3_club_pool[i];
            if(c->player.nationality!=national->nationality ||
               memcmp(c->player.playerName,national->playerName,
                      sizeof national->playerName)) continue;
            if(c->club==lookup[slot*2u] && c->slot==lookup[slot*2u+1u])
                currentMatch=1;
            matchClub=c->club;
            matchSlot=c->slot;
            matching++;
        }
        if(currentMatch) {kept++;continue;}
        if(matching==1u) {
            unsigned oldClub=lookup[slot*2u],oldSlot=lookup[slot*2u+1u];
            lookup[slot*2u]=matchClub;
            lookup[slot*2u+1u]=matchSlot;
            relinked++;
            SWOS_NationalTrace("NATIONAL_SOURCE_RELINK","season=%u team=%04X slot=%u old=%04X:%u new=%04X:%u name=%.23s",
                 (unsigned)_currentSeasonShowing,(unsigned)_selTeamNumber,slot,
                 oldClub,oldSlot,(unsigned)matchClub,(unsigned)matchSlot,
                 (const char *)national->playerName);
            continue;
        }
        if(matching) {
            ambiguous++;
            SWOS_NationalTrace("NATIONAL_SOURCE_AMBIGUOUS","season=%u team=%04X slot=%u matches=%u name=%.23s",
                (unsigned)_currentSeasonShowing,(unsigned)_selTeamNumber,
                slot,matching,(const char *)national->playerName);
            continue;
        }
        /* Do not treat a source country omitted from the pool as evidence
         * of retirement. Those club files are deliberately not scanned. */
        if ((lookup[slot*2u]>>8)>=80u ||
            !v3_valid_club_file(lookup[slot*2u]>>8) ||
            !v3_pool_country_loaded[lookup[slot*2u]>>8]) {
            ambiguous++;
            SWOS_NationalTrace("NATIONAL_SOURCE_UNVERIFIED","season=%u team=%04X slot=%u club=%04X name=%.23s",
                (unsigned)_currentSeasonShowing,(unsigned)_selTeamNumber,
                slot,(unsigned)lookup[slot*2u],
                (const char *)national->playerName);
            continue;
        }
        removeMask |= (uint32_t)1u<<slot;
        missing++;
        SWOS_NationalTrace("NATIONAL_SOURCE_MISSING","season=%u team=%04X slot=%u source=%04X:%u name=%.23s action=remove",
            (unsigned)_currentSeasonShowing,(unsigned)_selTeamNumber,slot,
            (unsigned)lookup[slot*2u],(unsigned)lookup[slot*2u+1u],
            (const char *)national->playerName);
    }
    if (removeMask) {
        extern void SWOS_NationalSquadUnavailableNewsRecord(const PlayerFile *player);
        /* Capture player names before the removal routine marks slots 0xFF. */
        for (unsigned slot = 0; slot < 20u; ++slot) {
            if (removeMask & ((uint32_t)1u << slot)) {
                const PlayerFile *player = (const PlayerFile *)((const uint8_t *)&_careerTeam +
                    sizeof(TeamFile) + slot * sizeof(PlayerFile));
                if (player->playerName[0] &&
                    (uint8_t)player->playerName[0] != 0xFFu)
                    SWOS_NationalSquadUnavailableNewsRecord(player);
                SWOS_NationalTrace("NATIONAL_SQUAD_NEWS",
                    "season=%u team=%04X slot=%u type=unavailable name=%.23s",
                    (unsigned)_currentSeasonShowing,
                    (unsigned)_selTeamNumber, slot,
                    (const char *)player->playerName);
            }
        }
        Career_RemoveInternationalPlayersByMask(removeMask);
    }
    SWOS_NationalTrace("NATIONAL_SOURCE_RECONCILE","season=%u team=%04X inspected=%u kept=%u relinked=%u removed=%u ambiguous=%u mask=%08X",
        (unsigned)_currentSeasonShowing,(unsigned)_selTeamNumber,considered,
        kept,relinked,missing,ambiguous,(unsigned)removeMask);
}

#endif
