#include "modern_reserve_purchase.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char blocked[MODERN_ID_LEN];
int ModernRuntime_PlayerIsBound(const char *id) {return blocked[0]&&!strcmp(blocked,id);}
static void id(char dst[MODERN_ID_LEN],unsigned n) {
    snprintf(dst,MODERN_ID_LEN,"00000000-0000-4000-8000-%012x",n);
}
static void setup(ModernWorld *w) {
    unsigned i,j,k=100;TeamFile t={0};
    w->count=2;w->clubs=calloc(2,sizeof *w->clubs);assert(w->clubs);
    for(i=0;i<2;i++){
        char cid[MODERN_ID_LEN];id(cid,i+1);
        t.countryNumber=3;t.teamOrdinal=i;
        ModernRoster_Init(&w->clubs[i],cid,&t);
        for(j=0;j<25;j++){
            ModernRosterPlayer p={0};id(p.id,k++);p.age=22;
            p.swos.positionAndFace=j==0?0:(7u<<5);
            assert(ModernRoster_Add(&w->clubs[i],&p));
        }
    }
}
int main(void) {
    ModernWorld w={0};int32_t bank=1000000,bought=0,sold=0;
    char first[MODERN_ID_LEN],reserve[MODERN_ID_LEN];
    setup(&w);
    strcpy(first,w.clubs[0].players[1].id); /* original visible 16 */
    strcpy(reserve,w.clubs[0].players[20].id);
    assert(ModernPurchase_Transfer(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                  first,100000,&bank,&bought,0));
    assert(bank==900000 && bought==100000);
    assert(w.clubs[0].count==24 && w.clubs[1].count==26);
    assert(!ModernPurchase_Transfer(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                  first,100000,&bank,&bought,0));
    strcpy(blocked,reserve);
    assert(!ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                  reserve,50000,&bank,&bought));
    blocked[0]=0;
    assert(ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                  reserve,50000,&bank,&bought));
    assert(bank==850000 && bought==150000);
    assert(ModernPurchase_SellReserve(&w,w.clubs[1].club_id,w.clubs[0].club_id,
                  first,75000,&bank,&sold));
    assert(bank==925000 && sold==75000);
    assert(!ModernPurchase_SellReserve(&w,w.clubs[1].club_id,w.clubs[0].club_id,
                  first,75000,&bank,&sold));
    assert(ModernCareer_ValidateWorld(&w));
    assert(w.clubs[0].count+w.clubs[1].count==50);
    ModernWorld_Free(&w);
    puts("R8 unified original-16 + reserve transfers: PASS");
    return 0;
}
