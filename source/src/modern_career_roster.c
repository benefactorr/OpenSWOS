#include "modern_career_roster.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int player_role(const ModernRosterPlayer *p) {
    unsigned role=((unsigned)p->swos.positionAndFace >> 5)&7u;
    if(role==0) return 0; /* goalkeeper */
    if(role<=3) return 1; /* defenders */
    if(role==4||role==5||role==6) return 2; /* midfielders and wingers */
    return 3;            /* forwards */
}
static int picked(const size_t slots[16],size_t count,size_t candidate) {
    size_t i;
    for(i=0;i<count;i++)if(slots[i]==candidate)return 1;
    return 0;
}
static int best_for_role(const ModernRoster *r,int role,const size_t slots[16],
                         size_t n,size_t *chosen) {
    size_t i;int best=-1;size_t index=0;
    for(i=0;i<r->count;i++) {
        const ModernRosterPlayer *p=&r->players[i];
        int score;
        if(player_role(p)!=role||picked(slots,n,i))continue;
        /* Preserve price as a coarse SWOS value score, with stable order on ties.
           Newer AI optimisation may supply a more detailed skill evaluator. */
        score=(unsigned char)p->swos.price;
        if(score>best) {best=score;index=i;}
    }
    if(best<0)return 0;
    *chosen=index;return 1;
}
static int best_remaining(const ModernRoster *r,const size_t slots[16],size_t n,size_t *chosen) {
    size_t i;int best=-1;size_t index=0;
    for(i=0;i<r->count;i++){
        int score;
        if(picked(slots,n,i))continue;
        score=(unsigned char)r->players[i].swos.price;
        if(score>best){best=score;index=i;}
    }
    if(best<0)return 0;
    *chosen=index;return 1;
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
int ModernCareer_Select16(const ModernRoster *r,size_t slots[16]) {
    static const int formation[11]={0,1,1,1,1,2,2,2,2,3,3};
    size_t i,n=0,chosen;
    if(!r||!slots||r->count<16)return 0;
    if(ModernCareer_Preferred16(r,slots))return 1;
    /* If all fields are absent, choose the best available; never duplicate.
       Require at least one natural goalkeeper. */
    if(!best_for_role(r,0,slots,n,&chosen))return 0;
    slots[n++]=chosen;
    for(i=1;i<11;i++) {
        if(!best_for_role(r,formation[i],slots,n,&chosen)) {
            if(!best_remaining(r,slots,n,&chosen))return 0;
        }
        slots[n++]=chosen;
    }
    /* Bench: second GK where available, then best remaining players. */
    if(best_for_role(r,0,slots,n,&chosen))slots[n++]=chosen;
    while(n<16) {
        if(!best_remaining(r,slots,n,&chosen))return 0;
        slots[n++]=chosen;
    }
    return 1;
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
void ModernCareer_NameReplacement(const ModernWorld *world, PlayerFile *player,
                             const PlayerFile *retired, uint32_t seed)
{
    /* Recombine a given name and a surname from the same nationality. */
    const char *original = (const char *)retired->playerName;
    const char *space = strchr(original, ' ');
    int given_length = space ? (int)(space - original) : 0;
    const char *given = original;
    if (!given_length || given_length > 10 ||
        (given_length == 3 && !memcmp(given, "RES", 3))) {
        given = "A.";
        given_length = 2;
    }
    const char *surname = space ? space + 1 : original;
    if (!*surname || (*surname >= '0' && *surname <= '9')) surname = "NOVA";
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        seed = (seed + attempt + 1) * UINT32_C(1664525) + UINT32_C(1013904223);
        const ModernRoster *club = &world->clubs[seed % world->count];
        if (!club->count) continue;
        const ModernRosterPlayer *donor = &club->players[(seed >> 16) % club->count];
        if (donor->is_reserve || donor->swos.nationality != retired->nationality) continue;
        const char *name = (const char *)donor->swos.playerName;
        const char *last = strrchr(name, ' ');
        if (!last || !last[1] || !strcmp(last + 1, surname)) continue;
        surname = last + 1;
        break;
    }
    snprintf((char *)player->playerName, sizeof player->playerName, "%.*s %.*s",
             given_length, given, (int)sizeof player->playerName - given_length - 2, surname);
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
        /* Trialists can move to another club and retain their original ID. */
        for (size_t c = 0; world && c < world->count && !used; ++c) {
            const ModernRoster *roster = &world->clubs[c];
            for (size_t i = 0; i < roster->count; ++i)
                if (!strcmp(roster->players[i].id, loan.id)) { used = 1; break; }
        }
        /* The caller can also be building a staged roster outside the world. */
        for (size_t i = 0; i < club->count && !used; ++i)
            if (!strcmp(club->players[i].id, loan.id)) used = 1;
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
