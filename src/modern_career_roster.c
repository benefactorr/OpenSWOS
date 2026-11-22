#include "modern_career_roster.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned player_role(const ModernRosterPlayer *player)
{
    return (player->swos.positionAndFace & PF_POSITION_MASK) >> PF_POSITION_SHIFT;
}

static int picked(const size_t slots[16], size_t candidate)
{
    for (size_t i = 0; i < 16; ++i)
        if (slots[i] == candidate) return 1;
    return 0;
}

static size_t best_player(const ModernRoster *roster, const size_t slots[16], int role)
{
    size_t best = SIZE_MAX;
    int bestPrice = -1;
    for (size_t i = 0; i < roster->count; ++i) {
        const ModernRosterPlayer *player = &roster->players[i];
        if ((role >= 0 && player_role(player) != (unsigned)role) ||
            !player->swos.playerName[0] || (uint8_t)player->swos.playerName[0] == 255 ||
            picked(slots, i))
            continue;
        /* The 2026/27 value curve is monotonic over the stored price bands. */
        unsigned price = (uint8_t)player->swos.price;
        int comparable = price <= 49 ? (int)price : 0;
        if (comparable > bestPrice) {
            best = i;
            bestPrice = comparable;
        }
    }
    return best;
}

int ModernCareer_Preferred16(const ModernRoster *r,size_t slots[16]) {
    size_t i,j;
    if (!r || !slots || !r->has_preferred16) return 0;
    for (i=0;i<16;i++) {
        for(j=0;j<r->count;j++)
            if(!strcmp(r->players[j].id,r->preferred16[i])) break;
        if(j==r->count) return 0; /* transferred/retired player */
        slots[i]=j;
        for(j=0;j<i;j++) if(slots[i]==slots[j]) return 0;
    }
    return player_role(&r->players[slots[0]])==0;
}
int ModernCareer_SetPreferred16(ModernRoster *r,const size_t slots[16]) {
    size_t i,j;
    if(!r||!slots||r->count<16)return 0;
    for(i=0;i<16;i++) {
        if(slots[i]>=r->count)return 0;
        for(j=0;j<i;j++)if(slots[i]==slots[j])return 0;
    }
    if(player_role(&r->players[slots[0]])!=0)return 0;
    for(i=0;i<16;i++)memcpy(r->preferred16[i],r->players[slots[i]].id,MODERN_ID_LEN);
    r->has_preferred16=1;
    return 1;
}
void ModernCareer_ClearPreferred16(ModernRoster *r) {
    if(!r)return;
    r->has_preferred16=0;memset(r->preferred16,0,sizeof r->preferred16);
}
int ModernCareer_SelectAI16(const ModernRoster *roster, size_t slots[16])
{
    static const uint8_t formation[11] = {0, 1, 3, 3, 2, 4, 6, 6, 5, 7, 7};
    /* Position preferences follow SWOS's replacement search. */
    static const uint8_t alternatives[8][8] = {
        {0, 3, 6, 7, 1, 2, 4, 5},
        {1, 3, 2, 4, 6, 5, 7, 0},
        {2, 3, 1, 5, 6, 4, 7, 0},
        {3, 1, 2, 6, 4, 5, 7, 0},
        {4, 6, 5, 3, 1, 2, 7, 0},
        {5, 6, 4, 3, 2, 1, 7, 0},
        {6, 4, 5, 3, 1, 2, 7, 0},
        {7, 6, 4, 5, 3, 1, 2, 0},
    };
    if (!roster || !slots || roster->count < 16) return 0;
    for (size_t i = 0; i < 16; ++i) slots[i] = SIZE_MAX;

    slots[0] = best_player(roster, slots, 0);
    if (slots[0] == SIZE_MAX) return 0;
    /* Give every starter first claim on their natural position. Only then
     * fill gaps, so an early fallback cannot take a later exact match. */
    for (size_t priority = 0; priority < 8; ++priority) {
        for (size_t i = 1; i < 11; ++i)
            if (slots[i] == SIZE_MAX)
                slots[i] = best_player(roster, slots, alternatives[formation[i]][priority]);
    }
    for (size_t i = 1; i < 11; ++i)
        if (slots[i] == SIZE_MAX) return 0;

    slots[11] = best_player(roster, slots, 0);
    for (size_t i = 11; i < 16; ++i) {
        if (slots[i] == SIZE_MAX) slots[i] = best_player(roster, slots, -1);
        if (slots[i] == SIZE_MAX) return 0;
    }
    return 1;
}

int ModernCareer_Select16(const ModernRoster *roster, size_t slots[16])
{
    return ModernCareer_Preferred16(roster, slots) || ModernCareer_SelectAI16(roster, slots);
}
int ModernCareer_BeginMatch(const ModernRoster *r,const size_t slots[16],
                             TeamFileEx *match,ModernMatchSelection *selection) {
    ModernMatchSelection tmp={0};size_t i;
    if(!r||!slots||!match||!selection)return 0;
    if(!ModernRoster_MatchTeam(r,slots,match))return 0;
    memcpy(tmp.club_id,r->club_id,MODERN_ID_LEN);
    for(i=0;i<16;i++)memcpy(tmp.player_ids[i],r->players[slots[i]].id,MODERN_ID_LEN);
    *selection=tmp;return 1;
}
int ModernCareer_CommitMatch(ModernRoster *r,const TeamFileEx *match,
                             const ModernMatchSelection *s) {
    size_t i,j,indices[16];
    if(!r||!match||!s||strcmp(r->club_id,s->club_id))return 0;

    /* Full validation before first mutation: missing/duplicated IDs abort. */
    for(i=0;i<16;i++) {
        int found=0;
        for(j=0;j<r->count;j++)if(!strcmp(r->players[j].id,s->player_ids[i])) {
            indices[i]=j;found=1;break;
        }
        if(!found)return 0;
        for(j=0;j<i;j++)if(indices[i]==indices[j])return 0;
    }
    for(i=0;i<16;i++)r->players[indices[i]].swos=match->players[i];
    return 1;
}
static int generated_surname_allowed(const char *surname)
{
    return strcmp(surname, "MALDINI") && strcmp(surname, "CASSIRAGHI") &&
           strcmp(surname, "CHIESA") && strcmp(surname, "NOVA") &&
           (surname[0] < '0' || surname[0] > '9');
}

void ModernCareer_NameReplacement(const ModernWorld *world, PlayerFile *player,
                                  const PlayerFile *origin, uint32_t seed)
{
    /* Sample given names and surnames independently across the nationality.
     * Exclude generated players so their names cannot become future donors. */
    char given[12] = {(char)('A' + seed % 26), '.', 0};
    char surname[sizeof player->playerName] = "NOVA";
    char original[sizeof origin->playerName + 1];
    snprintf(original, sizeof original, "%.23s", origin->playerName);
    const char *oldSurname = strrchr(original, ' ');
    oldSurname = oldSurname ? oldSurname + 1 : original;

    unsigned givenCount = 0, surnameCount = 0;
    uint32_t givenState = seed ^ UINT32_C(0xB5297A4D);
    uint32_t surnameState = seed ^ UINT32_C(0x68E31DA4);
    for (size_t i = 0; i < world->count; ++i) {
        const ModernRoster *club = &world->clubs[i];
        for (size_t j = 0; j < club->count; ++j) {
            const ModernRosterPlayer *candidate = &club->players[j];
            if (candidate->is_reserve || candidate->swos.nationality != origin->nationality)
                continue;
            char donor[sizeof candidate->swos.playerName + 1];
            snprintf(donor, sizeof donor, "%.23s", candidate->swos.playerName);
            const char *firstSpace = strchr(donor, ' ');
            if (!firstSpace || donor[0] == (char)255 || !firstSpace[1])
                continue;
            size_t givenLength = (size_t)(firstSpace - donor);
            if (givenLength && givenLength < sizeof given && strncmp(donor, "RES ", 4) &&
                strncmp(donor, "TRIAL ", 6)) {
                givenState = givenState * UINT32_C(1664525) + UINT32_C(1013904223);
                if ((givenState >> 8) % ++givenCount == 0) {
                    memcpy(given, donor, givenLength);
                    given[givenLength] = 0;
                }
            }
            const char *last = strrchr(donor, ' ') + 1;
            if (!*last || !generated_surname_allowed(last) || !strcmp(last, oldSurname))
                continue;
            surnameState = surnameState * UINT32_C(22695477) + 1;
            if ((surnameState >> 8) % ++surnameCount == 0)
                snprintf(surname, sizeof surname, "%s", last);
        }
    }
    if (!surnameCount && oldSurname[0] && generated_surname_allowed(oldSurname))
        snprintf(surname, sizeof surname, "%s", oldSurname);
    snprintf(player->playerName, sizeof player->playerName, "%s %.*s", given,
             (int)(sizeof player->playerName - strlen(given) - 2), surname);
}

static void read_player_skills(const PlayerFile *player, unsigned attrs[7])
{
    attrs[0] = (uint8_t)player->passing & 15;
    attrs[1] = (uint8_t)player->velocityHeading >> 4;
    attrs[2] = (uint8_t)player->velocityHeading & 15;
    attrs[3] = (uint8_t)player->tacklingControl >> 4;
    attrs[4] = (uint8_t)player->tacklingControl & 15;
    attrs[5] = (uint8_t)player->speedFinishing >> 4;
    attrs[6] = (uint8_t)player->speedFinishing & 15;
}

static unsigned replacement_random(uint32_t *seed, unsigned limit)
{
    *seed ^= *seed << 13;
    *seed ^= *seed >> 17;
    *seed ^= *seed << 5;
    return *seed % limit;
}

void ModernCareer_ReplacementSkills(const ModernRoster *club, PlayerFile *player,
                                   const PlayerFile *retired, uint32_t seed)
{
    unsigned position = player->positionAndFace & PF_POSITION_MASK;
    unsigned sums[7] = {0}, peers = 0, price_sum = 0;
    if (!seed) seed = 1;

    for (unsigned pass = 0; pass < (retired || !position ? 1u : 2u) && !peers; ++pass) {
        for (size_t i = 0; i < club->count; ++i) {
            const PlayerFile *peer = &club->players[i].swos;
            unsigned role = peer->positionAndFace & PF_POSITION_MASK;
            if (peer == player || !peer->playerName[0] ||
                (uint8_t)peer->playerName[0] == UINT8_MAX ||
                (uint8_t)peer->price > (retired && !position ? 127 : 49))
                continue;
            if ((!pass && role != position) || (pass && !role)) continue;
            if (!retired && ((peer->positionAndFace & PF_LOAN) ||
                (uint8_t)peer->form == PLAYER_FORM_TRIALIST ||
                (uint8_t)peer->form == PLAYER_FORM_RESERVE)) continue;
            unsigned attrs[7];
            read_player_skills(peer, attrs);
            for (size_t k = 0; k < 7; ++k) sums[k] += attrs[k] > 7 ? 7 : attrs[k];
            price_sum += (uint8_t)peer->price;
            ++peers;
        }
    }

    unsigned peer_total = price_sum;
    if (position) {
        peer_total = 0;
        for (size_t k = 0; k < 7; ++k) peer_total += sums[k];
    }
    unsigned average = peers ? (peer_total + peers / 2) / peers : 0;
    int target;
    if (retired) {
        unsigned baseline = (uint8_t)retired->price;
        if (position) {
            unsigned attrs[7];
            read_player_skills(retired, attrs);
            baseline = 0;
            for (size_t k = 0; k < 7; ++k) baseline += attrs[k] & 7;
        }
        static const unsigned limits[][2] = {{80,120},{85,115},{85,110},{90,110},{90,105}};
        unsigned tier = baseline / 10;
        if (tier > 4) tier = 4;
        unsigned lo = limits[tier][0], hi = limits[tier][1];
        unsigned pct = lo + replacement_random(&seed, hi - lo + 1);
        target = (int)((baseline * pct + 50) / 100);
        if (peers) {
            int adjustment = ((int)average - (int)baseline) / 10;
            if (adjustment > 2) adjustment = 2;
            if (adjustment < -2) adjustment = -2;
            target += adjustment;
            int lower = (int)((baseline * lo + 50) / 100);
            int upper = (int)((baseline * hi + 50) / 100);
            if (target < lower) target = lower;
            if (target > upper) target = upper;
        }
        if (target < 1) target = 1;
    } else {
        target = (int)average - 5 - (int)replacement_random(&seed, 6);
        if (target < 0) target = 0;
    }
    int limit = retired && !position ? 127 : 49;
    if (target > limit) target = limit;

    unsigned attrs[7] = {0};
    for (int point = 0; position && point < target; ++point) {
        size_t best = 0;
        int best_score = -1000;
        for (size_t k = 0; k < 7; ++k) {
            if (attrs[k] >= 7) continue;
            int preference = peers ? (int)(sums[k] * 8 / peers) : 24;
            int score = preference - (int)attrs[k] * 8 +
                        (int)replacement_random(&seed, 17);
            if (score > best_score) {
                best = k;
                best_score = score;
            }
        }
        ++attrs[best];
    }
    player->passing = (int8_t)attrs[0];
    player->velocityHeading = (int8_t)((attrs[1] << 4) | attrs[2]);
    player->tacklingControl = (int8_t)((attrs[3] << 4) | attrs[4]);
    player->speedFinishing = (int8_t)((attrs[5] << 4) | attrs[6]);
    player->price = (int8_t)target;
}

ModernRosterPlayer ModernCareer_NewLoan(const ModernWorld *world, const ModernRoster *club,
                                       const PlayerFile *origin, unsigned position, int shirt)
{
    ModernRosterPlayer loan = {0};
    unsigned team = (club->header.countryNumber << 8) | club->header.teamOrdinal;
    unsigned serial = 0;
    int used;
    do {
        snprintf(loan.id, sizeof loan.id, "53574c4e-%04x-8000-8000-%012x", team, ++serial);
        used = 0;
        /* Generated players can change clubs while retaining their UUID.
         * Checking only this club allows a later replacement to reuse an ID
         * already owned by a different club, permanently blocking sales. */
        if (world) {
            for (size_t c = 0; c < world->count && !used; ++c)
                for (size_t i = 0; i < world->clubs[c].count; ++i)
                    if (!strcmp(world->clubs[c].players[i].id, loan.id)) {
                        used = 1;
                        break;
                    }
        } else {
            for (size_t i = 0; i < club->count; ++i)
                if (!strcmp(club->players[i].id, loan.id)) { used = 1; break; }
        }
    } while (used);
    if (shirt <= 0) {
        for (shirt = 1; shirt < 100; ++shirt) {
            used = 0;
            for (size_t i = 0; i < club->count; ++i)
                if (club->players[i].swos.shirtNumber == shirt) { used = 1; break; }
            if (!used) break;
        }
    }
    loan.age = 17;
    loan.is_reserve = 1;
    loan.swos.nationality = origin->nationality;
    loan.swos.shirtNumber = (int8_t)shirt;
    loan.swos.positionAndFace = (uint8_t)(position | PF_LOAN);
    uint32_t seed = (team << 16) | serial;
    ModernCareer_NameReplacement(world, &loan.swos, origin, seed);
    ModernCareer_ReplacementSkills(club, &loan.swos, NULL, seed);
    return loan;
}

static int transfer(ModernWorld *w, const char *pid, const char *from, const char *to,
                    int allow_loan) {
    if(!w||!pid||!from||!to||!strcmp(from,to))return 0;
    ModernRoster *seller=ModernWorld_Find(w,from), *buyer=ModernWorld_Find(w,to);
    if(!seller||!buyer||seller->count<MODERN_MATCH_PLAYERS)return 0;
    size_t index;
    for(index=0;index<seller->count;index++)
        if(!strcmp(seller->players[index].id,pid))break;
    if(index==seller->count)return 0;
    ModernRosterPlayer player=seller->players[index];
    if (player.swos.positionAndFace & PF_LOAN) return 0;
    for(size_t j=0;j<w->count;j++) {
        const ModernRoster *r=&w->clubs[j];
        if(r==seller)continue;
        for(size_t i=0;i<r->count;i++)if(!strcmp(r->players[i].id,pid))return 0;
    }
    int needs_cover = seller->count == MODERN_MATCH_PLAYERS;
    if (!needs_cover && player_role(&player)==0) {
        size_t keepers=0;
        for (size_t i=0;i<seller->count;i++) keepers += player_role(&seller->players[i])==0;
        needs_cover = keepers < 2;
    }
    if (needs_cover && !allow_loan) return 0;
    ModernRosterPlayer cover = {0};
    if (needs_cover)
        cover = ModernCareer_NewLoan(w, seller, &player.swos,
                     player.swos.positionAndFace & PF_POSITION_MASK, player.swos.shirtNumber);
    if(!ModernRoster_Add(buyer,&player))return 0;
    if (needs_cover) {
        for (size_t i=0; seller->has_preferred16 && i<MODERN_MATCH_PLAYERS; ++i)
            if (!strcmp(seller->preferred16[i], player.id))
                memcpy(seller->preferred16[i], cover.id, MODERN_ID_LEN);
        seller->players[index] = cover;
    } else {
        ModernRoster_Remove(seller,player.id);
    }
    return 1;
}

int ModernCareer_Transfer(ModernWorld *w,const char *pid,const char *from,const char *to)
{
    return transfer(w,pid,from,to,0);
}

int ModernCareer_SellWithLoanCover(ModernWorld *w,const char *pid,const char *from,const char *to)
{
    return transfer(w,pid,from,to,1);
}

static int cmp_ids(const void *pa,const void *pb) {
    const char * const *a=(const char * const *)pa;
    const char * const *b=(const char * const *)pb;
    return strcmp(*a,*b);
}
int ModernCareer_ValidateWorld(const ModernWorld *w) {
    size_t a,i,total=0,used=0;
    const char **ids;
    if(!w||!w->count)return 0;
    for(a=0;a<w->count;a++) {
        size_t other;
        const ModernRoster *r=&w->clubs[a];int goalkeeper=0;
        for(other=0;other<a;other++)
            if(!strcmp(w->clubs[other].club_id,r->club_id))return 0;
        if(r->count<16||!r->players||total>SIZE_MAX-r->count)return 0;
        total+=r->count;
        for(i=0;i<r->count;i++)if(player_role(&r->players[i])==0)goalkeeper=1;
        if(!goalkeeper)return 0;
    }
    if(total>SIZE_MAX/sizeof(*ids))return 0;
    ids=malloc(total*sizeof *ids);if(!ids)return 0;
    for(a=0;a<w->count;a++) {
        const ModernRoster *r=&w->clubs[a];
        for(i=0;i<r->count;i++)ids[used++]=r->players[i].id;
    }
    qsort(ids,total,sizeof *ids,cmp_ids);
    for(i=1;i<total;i++)if(!strcmp(ids[i-1],ids[i])){free(ids);return 0;}
    free(ids);
    return 1;
}
int ModernCareer_SaveSnapshot(const ModernWorld *w,const char *path) {
    if(!ModernCareer_ValidateWorld(w))return 0;
    return ModernWorld_Save(w,path);
}
