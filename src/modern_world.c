#include "modern_world.h"
#include "modern_career_roster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static ModernWorld s_active_world={0};
ModernWorld *ModernWorld_Active(void) {return &s_active_world;}

#define MAX_WORLD_CLUBS 100000u
#define MAX_ROSTER_PLAYERS 10000u
static const unsigned char MAGIC_OLD[8]={'S','W','2','7','R','0','2',0};
static const unsigned char MAGIC_R7[8]={'S','W','2','7','R','0','7',0};
static const unsigned char MAGIC[8]={'S','W','2','7','R','0','8',0};

static int write32(FILE *f, uint32_t n) {
    unsigned char b[4]={(unsigned char)n,(unsigned char)(n>>8),
      (unsigned char)(n>>16),(unsigned char)(n>>24)};
    return fwrite(b,1,4,f)==4;
}
static int read32(FILE *f,uint32_t *n) {
    unsigned char b[4];
    if (fread(b,1,4,f)!=4)return 0;
    *n=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
    return 1;
}
static int valid_id(const char s[MODERN_ID_LEN]) {
    size_t i;
    if (s[36])return 0;
    for(i=0;i<36;i++) {
        char c=s[i];
        if(i==8||i==13||i==18||i==23) { if(c!='-') return 0; }
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;
    }
    return 1;
}
void ModernWorld_Free(ModernWorld *w) {
    size_t i;
    if(!w)return;
    for(i=0;i<w->count;i++) ModernRoster_Free(&w->clubs[i]);
    free(w->clubs);free(w->swos_lookup);
    w->clubs=NULL;w->swos_lookup=NULL;w->count=0;w->ai_transfer_run=0;
}
ModernRoster *ModernWorld_Find(ModernWorld *w,const char *id) {
    size_t i;
    if(!w||!id)return NULL;
    for(i=0;i<w->count;i++)if(!strcmp(w->clubs[i].club_id,id))return &w->clubs[i];
    return NULL;
}
ModernRoster *ModernWorld_FindSwos(ModernWorld *w,unsigned country,unsigned ordinal) {
    size_t i;
    uint32_t key;
    if(!w||country>255u||ordinal>255u)return NULL;
    key=(uint32_t)((country<<8)|ordinal);
    if(!w->swos_lookup && w->count) {
        /* Legacy team IDs fit in 16 bits. Build a direct map once instead of
         * scanning thousands of clubs for each AI match team lookup. The
         * first club wins in the unusual event of duplicate legacy IDs. */
        uint32_t *table=calloc(65536u,sizeof *table);
        if(table) {
            for(i=0;i<w->count;i++) {
                const TeamFile *h=&w->clubs[i].header;
                uint32_t k=((uint32_t)h->countryNumber<<8)|h->teamOrdinal;
                if(!table[k])table[k]=(uint32_t)i+1u;
            }
            w->swos_lookup=table;
        }
    }
    if(w->swos_lookup) {
        uint32_t slot=w->swos_lookup[key];
        return slot&&slot<=w->count ? &w->clubs[slot-1u] : NULL;
    }
    for(i=0;i<w->count;i++) {
        const TeamFile *h=&w->clubs[i].header;
        if(h->countryNumber==country&&h->teamOrdinal==ordinal)return &w->clubs[i];
    }
    return NULL;
}
int ModernWorld_Match(const ModernWorld *w,const char *id,const size_t slots[16],TeamFileEx *out) {
    size_t i;
    if(!w||!id)return 0;
    for(i=0;i<w->count;i++)if(!strcmp(w->clubs[i].club_id,id))
        return ModernRoster_MatchTeam(&w->clubs[i],slots,out);
    return 0;
}
int ModernWorld_Load(ModernWorld *w,const char *path) {
    FILE *f=NULL;ModernWorld tmp={0};unsigned char magic[8];uint32_t n,i,j;int r7,r8;
    if(!w||!path)return 0;
    f=fopen(path,"rb"); if(!f)return 0;
    if(fread(magic,1,8,f)!=8)goto fail;
    r8=memcmp(magic,MAGIC,8)==0;
    r7=r8 || memcmp(magic,MAGIC_R7,8)==0;
    if(!r7 && memcmp(magic,MAGIC_OLD,8)!=0)goto fail;
    if(!read32(f,&n)||n>MAX_WORLD_CLUBS)goto fail;
    if(r8 && !read32(f,&tmp.ai_transfer_run))goto fail;
    if(n) {tmp.clubs=calloc(n,sizeof *tmp.clubs);if(!tmp.clubs)goto fail;}
    for(i=0;i<n;i++) {
        char id[MODERN_ID_LEN];TeamFile header;uint32_t count;
        ModernRoster *r=&tmp.clubs[i];
        tmp.count=i+1;
        if(fread(id,1,sizeof id,f)!=sizeof id||!valid_id(id)||
           fread(&header,1,sizeof header,f)!=sizeof header||
           !read32(f,&count)||count>MAX_ROSTER_PLAYERS)goto fail;
        { uint32_t prev;for(prev=0;prev<i;prev++)if(!strcmp(tmp.clubs[prev].club_id,id))goto fail; }
        ModernRoster_Init(r,id,&header);
        for(j=0;j<count;j++) {
            ModernRosterPlayer p={0};unsigned char state[2];
            if(fread(p.id,1,sizeof p.id,f)!=sizeof p.id||!valid_id(p.id)||
               fread(&p.swos,1,sizeof p.swos,f)!=sizeof p.swos||
               fread(state,1,2,f)!=2)goto fail;
            if(r8 && (!read32(f,&p.last_ai_transfer) ||
                      p.last_ai_transfer>tmp.ai_transfer_run))goto fail;
            p.age=state[0];p.is_reserve=state[1];
            if(p.age<14||p.age>60||p.is_reserve>1||!ModernRoster_Add(r,&p))goto fail;
        }
        if(r7) {
            unsigned char flag;
            uint32_t k;
            if(fread(&flag,1,1,f)!=1 || flag>1)goto fail;
            r->has_preferred16=flag;
            if(fread(r->preferred16,1,sizeof r->preferred16,f)!=sizeof r->preferred16)goto fail;
            if(flag) {
                size_t slots[16];
                /* IDs must be owned, distinct, and the first must be a GK. */
                for(k=0;k<16;k++)if(!valid_id(r->preferred16[k]))goto fail;
                if(!ModernCareer_Preferred16(r,slots))goto fail;
            }
        }
    }
    /* Reject trailing bytes, including partial data from a wrong exporter. */
    if(fgetc(f)!=EOF)goto fail;
    fclose(f);ModernWorld_Free(w);*w=tmp;return 1;
fail:
    if(f)fclose(f);
    ModernWorld_Free(&tmp);return 0;
}
int ModernWorld_Save(const ModernWorld *w,const char *path) {
    FILE *f=NULL;size_t i,j;char *temp=NULL;size_t len;int ok=0;
    if(!w||!path||w->count>MAX_WORLD_CLUBS)return 0;
    /* Initial roster snapshots are write-once to avoid accidental overwrites
     * of a career snapshot on either Windows or Linux. */
    { FILE *existing=fopen(path,"rb"); if(existing){fclose(existing);return 0;} }
    len=strlen(path);if(len>4090)return 0;
    temp=malloc(len+5);if(!temp)return 0;
    memcpy(temp,path,len);memcpy(temp+len,".tmp",5);
    f=fopen(temp,"wb");if(!f)goto done;
    if(fwrite(MAGIC,1,8,f)!=8||!write32(f,(uint32_t)w->count)||
       !write32(f,w->ai_transfer_run))goto done;
    for(i=0;i<w->count;i++) {
        const ModernRoster *r=&w->clubs[i];
        if(!valid_id(r->club_id)||r->count>MAX_ROSTER_PLAYERS||
           fwrite(r->club_id,1,MODERN_ID_LEN,f)!=MODERN_ID_LEN||
           fwrite(&r->header,1,sizeof r->header,f)!=sizeof r->header||
           !write32(f,(uint32_t)r->count))goto done;
        for(j=0;j<r->count;j++) {
            const ModernRosterPlayer *p=&r->players[j];
            unsigned char state[2]={p->age,p->is_reserve};
            if(!valid_id(p->id)||p->age<14||p->age>60||p->is_reserve>1||
              fwrite(p->id,1,MODERN_ID_LEN,f)!=MODERN_ID_LEN||
              fwrite(&p->swos,1,sizeof p->swos,f)!=sizeof p->swos||
              fwrite(state,1,2,f)!=2||p->last_ai_transfer>w->ai_transfer_run||
              !write32(f,p->last_ai_transfer))goto done;
        }
        {
            unsigned char flag=r->has_preferred16;
            size_t slots[16];
            if(flag>1 || (flag && !ModernCareer_Preferred16(r,slots)) ||
               fwrite(&flag,1,1,f)!=1 ||
               fwrite(r->preferred16,1,sizeof r->preferred16,f)!=sizeof r->preferred16)
                goto done;
        }
    }
    if(fflush(f)!=0)goto done;
    if(fclose(f)!=0){f=NULL;goto done;}f=NULL;
#ifdef _WIN32
    /* On Windows rename() does not replace existing files. Leave the
     * original intact rather than deleting it before replacement. */
    if(rename(temp,path)!=0)goto done;
#else
    if(rename(temp,path)!=0)goto done;
#endif
    ok=1;
done:
    if(f)fclose(f);
    if(!ok)remove(temp);
    free(temp);return ok;
}
