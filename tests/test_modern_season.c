#include "modern_season.h"
#include "modern_career_roster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void build(ModernRoster *r,unsigned ordinal,unsigned size,unsigned country) {
    TeamFile h={0};char club[MODERN_ID_LEN];unsigned i;
    snprintf(club,sizeof club,"%08x-0000-4000-8000-000000000000",ordinal);
    h.countryNumber=(uint8_t)country;h.teamOrdinal=(uint8_t)ordinal;
    ModernRoster_Init(r,club,&h);
    for(i=0;i<size;i++) {
        ModernRosterPlayer p={0};unsigned role;
        snprintf(p.id,sizeof p.id,"%08x-0000-4000-8000-%012x",ordinal,i+1);
        role=i==0||i==24?0:(i%7)+1;
        p.swos.positionAndFace=(uint8_t)(role<<5);
        p.swos.price=(int8_t)(i%40);
        p.swos.cardsInjuries = i % 2 ? 0x42 : 0x6c;
        p.age=25;
        assert(ModernRoster_Add(r,&p));
    }
}
int main(int argc,char **argv) {
    ModernWorld w={0},saved={0};ModernSeasonStats stats={0};
    size_t original[4],all=0,i,j;char **ids;size_t used=0;
    assert(argc==2);
    w.count=4;w.clubs=calloc(w.count,sizeof *w.clubs);assert(w.clubs);
    build(&w.clubs[0],1,31,4); build(&w.clubs[1],2,25,4);
    build(&w.clubs[2],3,25,8); build(&w.clubs[3],4,18,8);
    for(i=0;i<w.count;i++){original[i]=w.clubs[i].count;all+=original[i];}
    ids=calloc(all,sizeof *ids);assert(ids);
    for(i=0;i<w.count;i++)for(j=0;j<w.clubs[i].count;j++) {
        ids[used]=malloc(MODERN_ID_LEN);assert(ids[used]);
        strcpy(ids[used++],w.clubs[i].players[j].id);
    }
    assert(ModernSeason_Advance(&w,1,&stats));
    assert(stats.players==all && stats.clubs==4);
    assert(w.clubs[0].players[0].age == 26);
    for (i = 0; i < w.count; ++i)
        for (j = 0; j < w.clubs[i].count; ++j)
            assert(w.clubs[i].players[j].swos.cardsInjuries == 0);
    w.clubs[0].players[24].swos.cardsInjuries = 0x42;
    assert(ModernSeason_Advance(&w,2,&stats));
    assert(w.clubs[0].players[0].age == 27);
    assert(w.clubs[0].players[24].swos.cardsInjuries == 0);
    /* Ageing also applies to the managed club. */
    assert(ModernSeason_Advance(&w,3,&stats));
    assert(w.clubs[0].players[0].age == 28);
    for(i=0;i<w.count;i++)assert(w.clubs[i].count==original[i]);
    for(i=0;i<all;i++){
        unsigned present=0;
        for(j=0;j<w.count;j++) {
            size_t k;
            for(k=0;k<w.clubs[j].count;k++)
                present+=strcmp(ids[i],w.clubs[j].players[k].id)==0;
        }
        assert(present==1);free(ids[i]);
    }
    free(ids);
    /* Check entire 16-player match selection still works after 2 seasons. */
    for(i=0;i<w.count;i++){
        size_t slots[16];TeamFileEx match;
        assert(ModernCareer_Select16(&w.clubs[i],slots));
        assert(ModernRoster_MatchTeam(&w.clubs[i],slots,&match));
        for (j = 0; j < MODERN_MATCH_PLAYERS; ++j)
            assert(match.players[j].cardsInjuries == 0);
    }
    for (i=0;i<w.count;i++) w.clubs[i].players[0].age=60;
    assert(ModernSeason_Advance(&w,4,&stats));
    assert(stats.retired > 0 && stats.players == all);
    assert(ModernWorld_Save(&w,argv[1]));
    assert(ModernWorld_Load(&saved,argv[1]));
    assert(ModernCareer_ValidateWorld(&saved));
    assert(saved.count==4);
    for (i = 0; i < saved.count; ++i) {
        assert(saved.clubs[i].count == original[i]);
        for (j = 0; j < saved.clubs[i].count; ++j)
            assert(saved.clubs[i].players[j].swos.cardsInjuries == 0);
    }
    ModernWorld_Free(&saved);ModernWorld_Free(&w);
    puts("R5 two-season expanded native rosters: PASS");
    return 0;
}
