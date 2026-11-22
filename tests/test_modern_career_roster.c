#include "modern_career_roster.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static void build(ModernRoster *r,unsigned team,unsigned n) {
    TeamFile h={0};unsigned i;char club[37];
    snprintf(club,sizeof club,"%08x-0000-4000-8000-000000000000",team);
    h.countryNumber=1;h.teamOrdinal=(uint8_t)team;
    ModernRoster_Init(r,club,&h);
    for(i=0;i<n;i++) {
        ModernRosterPlayer p={0};unsigned role;
        snprintf(p.id,sizeof p.id,"%08x-0000-4000-8000-%012x",team,i+1);
        role=(i==0||i==24)?0:(i%7)+1;
        p.swos.positionAndFace=(uint8_t)(role<<5);
        p.swos.price=(int8_t)(i%40);
        p.age=(uint8_t)(17+i%18);
        p.is_reserve=(i>=16);
        assert(ModernRoster_Add(r,&p));
    }
}
int main(int argc,char **argv) {
    ModernWorld w={0},restored={0};ModernMatchSelection mapping={0};
    TeamFileEx match={0};size_t slots[16],i;char first[37];
    assert(argc==2);
    w.clubs=calloc(2,sizeof *w.clubs);assert(w.clubs);w.count=2;
    build(&w.clubs[0],1,31);build(&w.clubs[1],2,25);
    assert(ModernCareer_ValidateWorld(&w));
    assert(ModernCareer_Select16(&w.clubs[0],slots));
    assert(ModernCareer_BeginMatch(&w.clubs[0],slots,&match,&mapping));
    strcpy(first,mapping.player_ids[0]);
    assert(((unsigned)match.players[0].positionAndFace>>5)==0);
    match.players[0].form=42;
    assert(ModernCareer_CommitMatch(&w.clubs[0],&match,&mapping));
    for(i=0;i<w.clubs[0].count;i++)if(!strcmp(first,w.clubs[0].players[i].id))
        assert(w.clubs[0].players[i].swos.form==42);
    /* A bad mapping never partially mutates a roster. */
    strcpy(mapping.player_ids[1],"99999999-0000-4000-8000-000000000000");
    match.players[0].form=12;
    assert(!ModernCareer_CommitMatch(&w.clubs[0],&match,&mapping));
    for(i=0;i<w.clubs[0].count;i++)if(!strcmp(first,w.clubs[0].players[i].id))
        assert(w.clubs[0].players[i].swos.form==42);
    /* Different team transfer, no player destroyed or cloned. */
    assert(ModernCareer_Transfer(&w,first,w.clubs[0].club_id,w.clubs[1].club_id));
    assert(w.clubs[0].count==30 && w.clubs[1].count==26);
    assert(!ModernCareer_Transfer(&w,first,w.clubs[0].club_id,w.clubs[1].club_id));
    assert(ModernCareer_ValidateWorld(&w));
    assert(ModernCareer_SaveSnapshot(&w,argv[1]));
    assert(!ModernCareer_SaveSnapshot(&w,argv[1]));
    assert(ModernWorld_Load(&restored,argv[1]));
    assert(restored.clubs[0].count==30 && restored.clubs[1].count==26);
    assert(ModernCareer_ValidateWorld(&restored));
    ModernWorld_Free(&restored);ModernWorld_Free(&w);
    puts("native modern career roster cycle: PASS");return 0;
}
