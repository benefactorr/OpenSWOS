#include "modern_reserve_purchase.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

static void make_id(char out[37],unsigned n) {
    snprintf(out,37,"00000000-0000-0000-0000-%012x",n);
}
static void build(ModernWorld *w, int player_count) {
    size_t i,j;unsigned id=100;TeamFile t={0};
    w->count=2;w->clubs=calloc(2,sizeof *w->clubs);
    for(i=0;i<2;i++) {
        char cid[37];make_id(cid,(unsigned)i+1);
        t.countryNumber=1;t.teamOrdinal=(uint8_t)i;
        ModernRoster_Init(&w->clubs[i],cid,&t);
        for(j=0;j<(size_t)player_count;j++) {
            ModernRosterPlayer p={0};make_id(p.id,id++);
            p.age=22;p.swos.positionAndFace=j==0?0:(7u<<5);
            assert(ModernRoster_Add(&w->clubs[i],&p));
        }
    }
}
int main(void) {
    ModernWorld w={0};int32_t balance=500000,purchases=0;
    char id[37],keeper[37];int ok;
    build(&w,25);
    strcpy(id,w.clubs[0].players[20].id);
    assert(!ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,id,600000,&balance,&purchases));
    assert(balance==500000 && w.clubs[0].count==25 && w.clubs[1].count==25);
    /* The starter and reserve paths both refuse players currently on loan. */
    w.clubs[0].players[1].swos.positionAndFace |= PF_LOAN;
    assert(!ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,w.clubs[0].players[1].id,1000,&balance,&purchases));
    assert(ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,id,120000,&balance,&purchases));
    assert(w.clubs[0].count==24 && w.clubs[1].count==26 && balance==380000 && purchases==120000);
    assert(!ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,id,120000,&balance,&purchases));
    assert(ModernCareer_ValidateWorld(&w));
    {
        int32_t sales=0;
        char sold[37];
        /* The buying club can sell its extra player by immutable ID. */
        strcpy(sold,id);
        assert(ModernPurchase_SellReserve(&w,w.clubs[1].club_id,w.clubs[0].club_id,
                sold,80000,&balance,&sales));
        assert(balance==460000 && sales==80000);
        assert(w.clubs[0].count==25 && w.clubs[1].count==25);
        assert(!ModernPurchase_SellReserve(&w,w.clubs[1].club_id,w.clubs[0].club_id,
                sold,80000,&balance,&sales));
        assert(sales==80000 && ModernCareer_ValidateWorld(&w));
    }
    ModernWorld_Free(&w);
    build(&w,17);
    strcpy(keeper,w.clubs[0].players[16].id);
    /* Final goalkeeper removal must be rejected, even from reserves. */
    w.clubs[0].players[0].swos.positionAndFace=(7u<<5);
    w.clubs[0].players[16].swos.positionAndFace=0;
    ok=ModernPurchase_Reserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,keeper,1000,&balance,&purchases);
    assert(!ok && w.clubs[0].count==17);
    assert(ModernPurchase_SellReserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                keeper,1000,&balance,&purchases));
    assert(w.clubs[0].count==17 && w.clubs[1].count==18);
    assert(w.clubs[1].players[17].age==22 && !strcmp(w.clubs[1].players[17].id,keeper));
    assert((w.clubs[0].players[16].swos.positionAndFace & (PF_LOAN|PF_POSITION_MASK))==PF_LOAN);
    assert(w.clubs[0].players[16].age==17 && ModernCareer_ValidateWorld(&w));
    strcpy(id,w.clubs[0].players[15].id);
    assert(ModernPurchase_SellReserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                id,1000,&balance,&purchases));
    assert(w.clubs[0].count==16);
    strcpy(id,w.clubs[0].players[14].id);
    assert(ModernPurchase_SellReserve(&w,w.clubs[0].club_id,w.clubs[1].club_id,
                id,1000,&balance,&purchases));
    assert(w.clubs[0].count==16 && (w.clubs[0].players[14].swos.positionAndFace & PF_LOAN));
    assert(ModernCareer_ValidateWorld(&w));
    ModernWorld_Free(&w);
    puts("R7e reserve purchase and sale tests passed");
    return 0;
}
