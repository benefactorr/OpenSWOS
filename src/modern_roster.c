#include "modern_roster.h"
#include <stdlib.h>
#include <string.h>
void ModernRoster_Init(ModernRoster *r, const char *club_id, const TeamFile *header) {
    memset(r,0,sizeof *r);
    if (club_id) { strncpy(r->club_id,club_id,MODERN_ID_LEN-1);r->club_id[MODERN_ID_LEN-1]=0; }
    if (header) memcpy(&r->header,header,sizeof r->header);
}
int ModernRoster_Add(ModernRoster *r, const ModernRosterPlayer *p) {
    size_t i; ModernRosterPlayer *n;
    if (!r || !p || !p->id[0]) return 0;
    for (i=0;i<r->count;i++) if (!strcmp(r->players[i].id,p->id)) return 0;
    if (r->count==r->capacity) {
        size_t cap=r->capacity ? r->capacity*2 : 32;
        if (cap<r->capacity || cap>SIZE_MAX/sizeof *n) return 0;
        n=realloc(r->players,cap*sizeof *n); if (!n) return 0;
        r->players=n;r->capacity=cap;
    }
    r->players[r->count++]=*p; return 1;
}
int ModernRoster_Remove(ModernRoster *r,const char *id) {
    size_t i;
    if (!r || !id) return 0;
    for (i=0;i<r->count;i++) if (!strcmp(r->players[i].id,id)) {
        size_t k;
        if(r->has_preferred16) for(k=0;k<MODERN_MATCH_PLAYERS;k++)
            if(!strcmp(r->preferred16[k],id)) {r->has_preferred16=0;break;}
        memmove(&r->players[i],&r->players[i+1],(r->count-i-1)*sizeof *r->players);
        --r->count; return 1;
    }
    return 0;
}
void ModernRoster_Free(ModernRoster *r) { if (r) {free(r->players);memset(r,0,sizeof *r);} }
int ModernRoster_MatchTeam(const ModernRoster *r,const size_t slots[16],TeamFileEx *out) {
    size_t i,j; TeamFileEx result;
    if (!r || !slots || !out || r->count<16) return 0;
    memset(&result,0,sizeof result);
    memcpy(&result.team,&r->header,sizeof result.team);
    for (i=0;i<16;i++) {
        if (slots[i]>=r->count) return 0;
        for(j=0;j<i;j++) if(slots[j]==slots[i]) return 0;
        result.players[i]=r->players[slots[i]].swos;
    }
    *out=result; return 1;
}
