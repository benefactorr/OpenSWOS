#include "modern_career_save.h"
#include "swos_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* This test runs in its own temporary directory. */
const char *swos_save_dir(void) { return "."; }
static void write_text(const char *p,const char *s) {
    FILE *f=fopen(p,"wb"); assert(f);
    assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));
}
static void expect_text(const char *p,const char *s) {
    FILE *f=fopen(p,"rb");char buf[128]={0};size_t n;
    assert(f);n=fread(buf,1,sizeof(buf)-1,f);fclose(f);
    assert(n==strlen(s)&&!memcmp(buf,s,n));
}
int main(void) {
    const unsigned char name[]="check.CAR";
    const char *car="./check.CAR";
    ModernWorld valid={0},bad={0},loaded={0};
    TeamFile h={0};
    int i;
    valid.count=1;valid.clubs=calloc(1,sizeof *valid.clubs);assert(valid.clubs);
    ModernRoster_Init(valid.clubs,"00000000-0000-0000-0000-000000000001",&h);
    for(i=0;i<25;i++) {
        ModernRosterPlayer p={0};
        snprintf(p.id,sizeof(p.id),"00000000-0000-0000-0000-%012x",i+100);
        p.age=20;p.swos.positionAndFace=i? (7u<<5):0;
        assert(ModernRoster_Add(valid.clubs,&p));
    }
    write_text(car,"career-old");
    assert(ModernSave_Write(name,&valid));
    assert(ModernSave_PrepareLoad(name,&loaded));
    ModernWorld_Free(&loaded);
    assert(ModernSave_BeginCheckpoint(name));
    write_text(car,"career-new");
    /* Intentionally invalid club ID forces expanded roster write failure. */
    bad.count=1;bad.clubs=calloc(1,sizeof *bad.clubs);assert(bad.clubs);
    ModernRoster_Init(bad.clubs,"invalid",&h);
    assert(!ModernSave_Write(name,&bad));
    /* Simulate interrupted sidecar/link replacement as well. */
    write_text("./check.CAR.r04", "broken-roster");
    write_text("./check.CAR.r04.link", "broken-link");
    assert(ModernSave_RollbackCheckpoint());
    expect_text(car,"career-old");
    assert(ModernSave_PrepareLoad(name,&loaded));
    assert(loaded.count==1&&loaded.clubs[0].count==25);
    assert(ModernSave_BeginCheckpoint(name));
    write_text(car,"career-new");
    assert(ModernSave_Write(name,&valid));
    ModernSave_EndCheckpoint();
    assert(ModernSave_PrepareLoad(name,&loaded));
    assert(loaded.clubs[0].count==25);
    ModernWorld_Free(&loaded);
    ModernWorld_Free(&valid);ModernWorld_Free(&bad);
    puts("R7e checkpoint rollback and saved roster tests passed");
    return 0;
}
