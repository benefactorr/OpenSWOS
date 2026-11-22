#include "modern_season.h"
#include "modern_career_roster.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    ModernWorld w={0},loaded={0};ModernSeasonStats st={0};size_t i,n=0;
    if(argc!=3||!ModernWorld_Load(&w,argv[1])||!ModernCareer_ValidateWorld(&w))return 1;
    for(i=0;i<w.count;i++)n+=w.clubs[i].count;
    if(!ModernSeason_Advance(&w,1,&st)||st.players!=n)return 2;
    if(!ModernSeason_Advance(&w,2,&st)||st.players!=n)return 3;
    if(!ModernWorld_Save(&w,argv[2]))return 4;
    if(!ModernWorld_Load(&loaded,argv[2])||!ModernCareer_ValidateWorld(&loaded))return 5;
    if(loaded.count!=w.count)return 6;
    for(i=0;i<w.count;i++)if(w.clubs[i].count!=loaded.clubs[i].count)return 7;
    printf("R5 real-world roster rollover: PASS clubs=%lu players=%lu\n",
       (unsigned long)w.count,(unsigned long)n);
    ModernWorld_Free(&w);ModernWorld_Free(&loaded);return 0;
}
