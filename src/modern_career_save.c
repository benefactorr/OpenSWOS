#include "modern_career_save.h"
#include "swos_io.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a detects stale or mismatched paired files; this is NOT an authenticity
 * signature. The native game refuses a mismatched sidecar rather than dropping
 * the AI reserves and continuing with only sixteen players. */
static int fingerprint(const char *path,uint64_t *fingerprint_out) {
    FILE *f=fopen(path,"rb");unsigned char b[8192];size_t n,i;
    uint64_t h=UINT64_C(14695981039346656037);
    if(!f)return 0;
    while((n=fread(b,1,sizeof b,f))!=0)
        for(i=0;i<n;i++){h^=b[i];h*=UINT64_C(1099511628211);}
    if(ferror(f)){fclose(f);return 0;}
    if(fclose(f)!=0)return 0;
    *fingerprint_out=h;
    return 1;
}
static int paths(const unsigned char *name,char *career,char *roster,char *link) {
    const char *base,*colon;size_t n;
    if(!name||!name[0])return 0;
    base=(const char *)name;
    colon=strchr(base,':');if(colon)base=colon+1;
    /* Saving one selected filename, not paths chosen by foreign metadata. */
    if(!*base||strchr(base,'/')||strchr(base,'\\')||strstr(base,".."))return 0;
    n=strlen(swos_save_dir())+strlen(base)+8;
    if(n>=1024)return 0;
    snprintf(career,1024,"%s/%s",swos_save_dir(),base);
    snprintf(roster,1024,"%s.r04",career);
    snprintf(link,1024,"%s.link",roster);
    return 1;
}
static void suffix(char *dst,const char *src,const char *s) {
    snprintf(dst,1050,"%s%s",src,s);
}
int ModernSave_Write(const unsigned char *name,const ModernWorld *world) {
    char car[1024],r04[1024],link[1024],next[1050],nextlink[1050],backup[1050];
    uint64_t hc,hr;FILE *f;int had_old=0;
    if(!world||!paths(name,car,r04,link))return 0;
    if(!fingerprint(car,&hc))return 0;
    suffix(next,r04,".next");suffix(nextlink,link,".next");suffix(backup,r04,".previous");
    remove(next);remove(nextlink);
    if(!ModernWorld_Save(world,next))return 0;
    if(!fingerprint(next,&hr)){remove(next);return 0;}
    f=fopen(nextlink,"wb");
    if(!f){remove(next);return 0;}
    {
        int printed=fprintf(f,"SWOS2027-R04 %016llx %016llx\n",
          (unsigned long long)hc,(unsigned long long)hr);
        int closed=fclose(f);
        if(printed<0 || closed!=0){
            remove(next);remove(nextlink);return 0;
        }
    }
    /* Prepare a recoverable previous roster. On Windows rename() cannot
       replace an existing destination: move it aside before committing. */
    remove(backup);
    f=fopen(r04,"rb");if(f){had_old=1;fclose(f);if(rename(r04,backup)!=0){remove(next);remove(nextlink);return 0;}}
    if(rename(next,r04)!=0){
        if(had_old)rename(backup,r04);
        remove(next);remove(nextlink);return 0;
    }
    /* Link is written last. If interrupted, load will reject inconsistent
       pairings rather than silently dropping players. */
    remove(link);
    if(rename(nextlink,link)!=0){
        remove(r04);
        if(had_old)rename(backup,r04);
        return 0;
    }
    return 1;
}
int ModernSave_PrepareLoad(const unsigned char *name,ModernWorld *staged) {
    char car[1024],r04[1024],link[1024],magic[32];
    unsigned long long ec=0,er=0;uint64_t hc,hr;
    FILE *f;ModernWorld tmp={0};
    if(!staged||!paths(name,car,r04,link))return 0;
    f=fopen(link,"rb");if(!f)return 0;
    if(fscanf(f,"%31s %llx %llx",magic,&ec,&er)!=3 ||
       strcmp(magic,"SWOS2027-R04")){fclose(f);return 0;}
    fclose(f);
    if(!fingerprint(car,&hc)||!fingerprint(r04,&hr)||
       hc!=(uint64_t)ec||hr!=(uint64_t)er)return 0;
    if(!ModernWorld_Load(&tmp,r04))return 0;
    ModernWorld_Free(staged);*staged=tmp;
    return 1;
}

/* R7e: protect the previous consistent .CAR / expanded-roster trio
 * while the legacy JSON writer and roster writer run separately. */
static int checkpoint_active=0;
static int checkpoint_old=0;
static char checkpoint_paths[3][1024];
static char checkpoint_backups[3][1050];

static int exists_file(const char *path) {
    FILE *f=fopen(path,"rb");
    if(f){fclose(f);return 1;}
    return 0;
}
static int copy_bytes(const char *from,const char *to) {
    FILE *input=fopen(from,"rb"),*output;unsigned char block[16384];size_t n;int ok=1;
    if(!input)return 0;
    output=fopen(to,"wb");if(!output){fclose(input);return 0;}
    while((n=fread(block,1,sizeof block,input))>0)
        if(fwrite(block,1,n,output)!=n){ok=0;break;}
    if(ferror(input))ok=0;
    if(fflush(output)!=0)ok=0;
    if(fclose(output)!=0)ok=0;
    fclose(input);
    if(!ok)remove(to);
    return ok;
}
int ModernSave_BeginCheckpoint(const unsigned char *name) {
    char car[1024],roster[1024],link[1024];
    int i,j,has_car,has_roster,has_link;
    if(checkpoint_active||!paths(name,car,roster,link))return 0;
    strcpy(checkpoint_paths[0],car);
    strcpy(checkpoint_paths[1],roster);
    strcpy(checkpoint_paths[2],link);
    for(i=0;i<3;i++) {
        suffix(checkpoint_backups[i],checkpoint_paths[i],".modern-rollback");
        /* Never overwrite evidence of an interrupted save. */
        if(exists_file(checkpoint_backups[i]))return 0;
    }
    has_car=exists_file(car);has_roster=exists_file(roster);
    has_link=exists_file(link);
    /* A save with a missing component must not be overwritten silently. */
    if((has_car!=has_roster)||(has_roster!=has_link))return 0;
    checkpoint_old=has_car;
    if(checkpoint_old) {
        for(i=0;i<3;i++)if(!copy_bytes(checkpoint_paths[i],checkpoint_backups[i])) {
            for(j=0;j<i;j++)remove(checkpoint_backups[j]);
            return 0;
        }
    }
    checkpoint_active=1;
    return 1;
}
void ModernSave_EndCheckpoint(void) {
    int i;
    if(!checkpoint_active)return;
    if(checkpoint_old)for(i=0;i<3;i++)remove(checkpoint_backups[i]);
    checkpoint_active=0;
}
int ModernSave_RollbackCheckpoint(void) {
    int i,ok=1;
    if(!checkpoint_active)return 0;
    for(i=0;i<3;i++) {
        if(checkpoint_old) {
            if(!copy_bytes(checkpoint_backups[i],checkpoint_paths[i]))ok=0;
        } else if(exists_file(checkpoint_paths[i]) &&
                  remove(checkpoint_paths[i])!=0)ok=0;
    }
    if(ok && checkpoint_old)
        for(i=0;i<3;i++)remove(checkpoint_backups[i]);
    checkpoint_active=0;
    return ok;
}
