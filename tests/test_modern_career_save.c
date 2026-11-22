#include "modern_career_save.h"
#include "modern_career_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *folder;
const char *swos_save_dir(void) {return folder;}
static void write_car(const char *dir,const char *body) {
    char p[1024];FILE *f;
    snprintf(p,sizeof p,"%s/TEST.CAR",dir);
    f=fopen(p,"wb");assert(f);
    assert(fwrite(body,1,strlen(body),f)==strlen(body));
    assert(fclose(f)==0);
}
int main(int argc,char **argv) {
    ModernWorld w={0},loaded={0};ModernRosterPlayer p={0};TeamFile h={0};
    unsigned i;
    assert(argc==2);folder=argv[1];
    write_car(folder,"{\"season\":1}");
    w.clubs=calloc(1,sizeof *w.clubs);assert(w.clubs);w.count=1;
    w.ai_transfer_run=4;
    h.countryNumber=9;h.teamOrdinal=2;
    ModernRoster_Init(w.clubs,"11111111-1111-4111-8111-111111111111",&h);
    for(i=0;i<31;i++) {
        snprintf(p.id,sizeof p.id,"22222222-2222-4222-8222-%012x",i);
        p.last_ai_transfer=i==0?3:0;
        p.age=20;p.swos.price=(int8_t)i;
        p.swos.positionAndFace=(uint8_t)((i==0?0:1)<<5);
        assert(ModernRoster_Add(w.clubs,&p));
    }
    assert(ModernSave_Write((const unsigned char *)"TEST.CAR",&w));
    assert(ModernSave_PrepareLoad((const unsigned char *)"TEST.CAR",&loaded));
    assert(loaded.count==1&&loaded.clubs[0].count==31);
    assert(loaded.ai_transfer_run==4 && loaded.clubs[0].players[0].last_ai_transfer==3);
    ModernWorld_Free(&loaded);
    /* The R04 pairing is deliberately sensitive to changed career bytes. */
    write_car(folder,"{\"season\":2}");
    assert(!ModernSave_PrepareLoad((const unsigned char *)"TEST.CAR",&loaded));
    assert(ModernSave_Write((const unsigned char *)"TEST.CAR",&w));
    assert(ModernSave_PrepareLoad((const unsigned char *)"TEST.CAR",&loaded));
    /* A corrupt roster is also rejected before the legacy save is loaded. */
    {
        char path[1024];FILE *f;
        snprintf(path,sizeof path,"%s/TEST.CAR.r04",folder);
        f=fopen(path,"ab");assert(f);fputc('X',f);fclose(f);
    }
    ModernWorld_Free(&loaded);
    assert(!ModernSave_PrepareLoad((const unsigned char *)"TEST.CAR",&loaded));
    ModernWorld_Free(&w);
    puts("native per-career roster save/load: PASS");return 0;
}
