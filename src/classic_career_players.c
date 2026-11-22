#ifdef SWOS_SDL_BUILD
#include "swos_age_identity_log.h"
#include "swos_ai_identity_journal.h"
#include "swos_identity_runtime.h"
#include "swos_identity_seed.h"
#include "ai_career_pool_capacity.h"
#include "world.h"
#include "game_types.h"
#include "swos_fixed_team_name.h"
#include "endian_helpers.h"
#include "rnc.h"
#include "competition_data.h"
#include "career_teams.h"
#include "swos_io.h"
#include "menu_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern uint16_t _selTeamNumber, _isNationalSelector, _currentSeasonShowing;
extern uint16_t _careerSeasonsCompleted, _numForeignMarketPlayers2;
extern uint16_t _careerNumPlayers;
extern TeamFileEx _careerTeam;
extern uint8_t _seasonPlaying;
extern uint16_t _pendingForeignPlayerCount, _lastLoadedExtNumber;
extern uint8_t _viewRandomPhase;
extern TeamFileEx *GetTeamPointer(uint16_t teamId);
extern void SWOS_AgesResetCareer(void);
extern void SWOS_AgesAdvanceOneSeason(void);
#define SWOS_AI_LOG(...) do { \
    FILE *swos_ai_file = fopen("swos_ai_debug.log", "a"); \
    if (swos_ai_file) { \
        fprintf(swos_ai_file, __VA_ARGS__); \
        fclose(swos_ai_file); \
    } \
} while (0)

#define SWOS_AI_P3_MAX_CLUBS 2048
#define SWOS_AI_P3_MAX_EXCHANGES 12000

typedef struct SWOS_AI_P3Club {
    uint16_t id;
    uint8_t sourceFile;
    uint8_t sourceRecord;
    uint8_t country;
    uint8_t league;
    char name[18];
    PlayerFile players[16];
    /* Stable cooldown identities travel with the physical player slots. */
    uint64_t cooldownKeys[16];
    /* The first eleven DISPLAY positions come from playerNumbers, not
     * physical player slots 0..10. Keep the TEAM file permutation. */
    uint8_t lineupSlots[16];
    uint8_t lineupValid;
    uint8_t usedSlots[16];
    uint8_t requested;
    uint8_t completed;
} SWOS_AI_P3Club;

static int SWOS_AI_P3Cmp(const void *aa, const void *bb)
{
    const SWOS_AI_P3Club *a = (const SWOS_AI_P3Club *)aa;
    const SWOS_AI_P3Club *b = (const SWOS_AI_P3Club *)bb;
    if (a->country != b->country) return (int)a->country - (int)b->country;
    if (a->league != b->league) return (int)a->league - (int)b->league;
    return strcmp(a->name, b->name);
}

/* Remember players moved in the preceding two button clicks.  The recent
 * keys are serialised by the existing JSON career hooks.  Prefer permanent
 * player IDs over record fingerprints, which change when injuries, goals or
 * other mutable PlayerFile fields change. */
#define SWOS_AI_P3_RECENT_SIZE 65536u
static uint64_t SWOS_AI_P3RecentKeys[SWOS_AI_P3_RECENT_SIZE];
static uint32_t SWOS_AI_P3RecentClicks[SWOS_AI_P3_RECENT_SIZE];
static uint32_t SWOS_AI_P3ClickNumber;

/* JSON career save hooks. Older saves without these keys reset only the
 * cooldown, never the actual transfer records. */
void SWOS_AI_P3ResetHistory(void)
{
    memset(SWOS_AI_P3RecentKeys, 0, sizeof(SWOS_AI_P3RecentKeys));
    memset(SWOS_AI_P3RecentClicks, 0, sizeof(SWOS_AI_P3RecentClicks));
    SWOS_AI_P3ClickNumber = 0;
}

uint32_t SWOS_AI_P3GetClickNumber(void) { return SWOS_AI_P3ClickNumber; }

unsigned SWOS_AI_P3GetRecent(unsigned start, uint32_t *hi, uint32_t *lo,
                              uint32_t *last)
{
    for (unsigned i = start; i < SWOS_AI_P3_RECENT_SIZE; ++i) {
        uint32_t n = SWOS_AI_P3RecentClicks[i];
        if (!n || (uint32_t)(SWOS_AI_P3ClickNumber - n) > 2u) continue;
        uint64_t key = SWOS_AI_P3RecentKeys[i];
        if (!key) continue;
        *hi = (uint32_t)(key >> 32); *lo = (uint32_t)key;
        *last = n;
        return i + 1u;
    }
    return 0;
}

static unsigned SWOS_AI_P3RecentIndex(uint64_t key, uint32_t click);

void SWOS_AI_P3LoadRecent(uint32_t hi, uint32_t lo, uint32_t last)
{
    uint64_t key = ((uint64_t)hi << 32) | lo;
    if (!key || !last || last > SWOS_AI_P3ClickNumber ||
        SWOS_AI_P3ClickNumber - last > 2u) return;
    unsigned i = SWOS_AI_P3RecentIndex(key, SWOS_AI_P3ClickNumber);
    SWOS_AI_P3RecentKeys[i] = key;
    SWOS_AI_P3RecentClicks[i] = last;
}

void SWOS_AI_P3SetClickNumber(uint32_t n) { SWOS_AI_P3ClickNumber = n; }


static uint64_t SWOS_AI_P3Fingerprint(PlayerFile p)
{
    const unsigned char *bytes = (const unsigned char *)&p;
    uint64_t h = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(p); i++) {
        h ^= bytes[i];
        h *= UINT64_C(1099511628211);
    }
    /* Reserve the upper bit for permanent-ID keys.  Existing older save
     * cooldown entries remain readable, although they will naturally expire
     * after the usual two AI-transfer clicks. */
    h &= UINT64_C(0x7FFFFFFFFFFFFFFF);
    return h ? h : 1;
}

/* The player-ID registry is already moved by SWOS_AI_P3Swap(), including
 * generated and manually transferred identities.  Resolve each slot before
 * executing a batch and then move this cached key with the player record.
 * Do not query the identity registry after swapping: that would obtain the
 * destination slot's occupant, not necessarily the transferred player. */
unsigned SWOS_IdentityAgeForClub(unsigned club, unsigned slot, unsigned *age, unsigned *id);
static uint64_t SWOS_AI_P3CooldownKey(unsigned club, unsigned slot, PlayerFile p)
{
    unsigned age = 0, identity = 0;
    if (SWOS_IdentityAgeForClub(club, slot, &age, &identity) && identity)
        return UINT64_C(0x8000000000000000) | (uint64_t)identity;
    return SWOS_AI_P3Fingerprint(p);
}

static unsigned SWOS_AI_P3RecentIndex(uint64_t key, uint32_t click)
{
    unsigned first = (unsigned)((key ^ (key >> 32)) & (SWOS_AI_P3_RECENT_SIZE - 1u));
    unsigned reusable = SWOS_AI_P3_RECENT_SIZE;
    for (unsigned probe = 0; probe < SWOS_AI_P3_RECENT_SIZE; ++probe) {
        unsigned i = (first + probe) & (SWOS_AI_P3_RECENT_SIZE - 1u);
        uint32_t last = SWOS_AI_P3RecentClicks[i];
        if (last && SWOS_AI_P3RecentKeys[i] == key)
            return i;  /* Prefer an existing key, even beyond an expired one. */
        if (!last) return reusable < SWOS_AI_P3_RECENT_SIZE ? reusable : i;
        if ((uint32_t)(click - last) > 2u && reusable == SWOS_AI_P3_RECENT_SIZE)
            reusable = i;
    }
    return reusable < SWOS_AI_P3_RECENT_SIZE ? reusable : first;
}

static int SWOS_AI_P3RecentlyMoved(uint64_t key, uint32_t click)
{
    unsigned i = SWOS_AI_P3RecentIndex(key, click);
    uint32_t last = SWOS_AI_P3RecentClicks[i];
    return SWOS_AI_P3RecentKeys[i] == key && last &&
           (uint32_t)(click - last) <= 2u;
}

static void SWOS_AI_P3MarkMoved(uint64_t key, uint32_t click)
{
    unsigned i = SWOS_AI_P3RecentIndex(key, click);
    SWOS_AI_P3RecentKeys[i] = key;
    SWOS_AI_P3RecentClicks[i] = click;
}

static uint32_t SWOS_AI_P3Seed = 0x534F5753u;
static uint32_t SWOS_AI_P3Random(void)
{
    SWOS_AI_P3Seed = SWOS_AI_P3Seed * 1664525u + 1013904223u;
    return SWOS_AI_P3Seed;
}

/* Use SWOS's own two-step price lookup, in game price units, rather than
 * comparing the raw price indices as if they were linear currency values. */
static unsigned SWOS_AI_P3Value(PlayerFile p)
{
    extern int8_t _avgTeamValues[];
    extern uint16_t _avgTeamValueBases[];
    int price = p.price;
    if (price < 0 || price > 49 || !p.playerName[0]) return 0;
    unsigned index = (uint8_t)_avgTeamValues[price];
    if (!index || index >= 92) return 0;
    return _avgTeamValueBases[index];
}

static unsigned SWOS_AI_P3Position(PlayerFile p)
{
    return (p.positionAndFace >> 5) & 7u;
}

static int SWOS_AI_P3IsValid(PlayerFile p)
{
    unsigned pos = SWOS_AI_P3Position(p);
    return pos <= 7 && SWOS_AI_P3Value(p) > 0;
}

/* All consumers of the foreign-market pool use the last matching slot.
 * A saved career can contain multiple historical entries for one slot. The
 * normal roster loader and world search apply them in order (last wins).
 * The AI transfer code must therefore never select the first entry. */
static int SWOS_AI_P3OverrideMatches(const MarketPlayer *p,
                                      uint16_t teamId, unsigned slot)
{
    const uint8_t *bytes = (const uint8_t *)&p->teamId;
    return bytes[0] == (uint8_t)(teamId >> 8) &&
           bytes[1] == (uint8_t)teamId &&
           (uint8_t)p->playerOrdinal == slot;
}

static unsigned SWOS_AI_P3OverrideIndex(uint16_t teamId, unsigned slot)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    unsigned latest = _numForeignMarketPlayers2;
    for (unsigned k = 0; k < _numForeignMarketPlayers2; ++k)
        if (SWOS_AI_P3OverrideMatches(&_careerForeignMarketPlayers[k], teamId, slot))
            latest = k;
    return latest;
}

/* No record is removed: historical saves and pool counters keep their layout.
 * All older claims for a written slot are updated so whichever consumer is
 * reading the market pool sees the same live player. */
static void SWOS_AI_P3WriteOverrideAt(uint16_t teamId, unsigned slot,
                                      PlayerFile incoming, unsigned index)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _lastLoadedExtNumber;
    extern uint8_t _viewRandomPhase;
    unsigned oldCount = _numForeignMarketPlayers2;
    MarketPlayer *p = &_careerForeignMarketPlayers[index];
    ((uint8_t *)&p->teamId)[0] = (uint8_t)(teamId >> 8);
    ((uint8_t *)&p->teamId)[1] = (uint8_t)teamId;
    p->playerOrdinal = (int8_t)slot;
    p->padding = (int8_t)_viewRandomPhase;
    p->player = incoming;
    if (index == oldCount) {
        ++_numForeignMarketPlayers2;
        ++_pendingForeignPlayerCount;
    }
    for (unsigned k = 0; k < oldCount; ++k) {
        if (k != index && SWOS_AI_P3OverrideMatches(
                &_careerForeignMarketPlayers[k], teamId, slot))
            _careerForeignMarketPlayers[k].player = incoming;
    }
    _lastLoadedExtNumber = 255;
}

/* Reconcile only the active prefix of the foreign-market pool.
 * The last _pendingForeignPlayerCount entries are future updates, and their
 * player records must NOT be merged with active entries for the same slot.
 * They may also contain successive updates for different random phases.
 * Keep indices, player identities, counts, and chronological order intact. */
#define SWOS_AI_OVERRIDE_HASH 131072u
static void SWOS_AI_P3ReconcileOverrides(void)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    uint32_t *keys = calloc(SWOS_AI_OVERRIDE_HASH, sizeof(*keys));
    uint32_t *latest = calloc(SWOS_AI_OVERRIDE_HASH, sizeof(*latest));
    unsigned repaired = 0, duplicates = 0;
    unsigned total = _numForeignMarketPlayers2;
    unsigned pending = _pendingForeignPlayerCount;
    unsigned activeEnd = total - (pending < total ? pending : total);
    if (!keys || !latest) { free(keys); free(latest); return; }

    for (unsigned i = 0; i < activeEnd; ++i) {
        const MarketPlayer *p = &_careerForeignMarketPlayers[i];
        if (p->playerOrdinal < 0 || p->playerOrdinal >= 16) continue;
        const uint8_t *b = (const uint8_t *)&p->teamId;
        uint32_t key = (((uint32_t)b[0] << 8) | b[1]) * 16u +
                       (unsigned)p->playerOrdinal + 1u;
        unsigned h = (key * 2654435761u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        while (keys[h] && keys[h] != key)
            h = (h + 1u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        if (keys[h]) ++duplicates;
        keys[h] = key;
        latest[h] = i;
    }
    for (unsigned i = 0; i < activeEnd; ++i) {
        MarketPlayer *p = &_careerForeignMarketPlayers[i];
        if (p->playerOrdinal < 0 || p->playerOrdinal >= 16) continue;
        const uint8_t *b = (const uint8_t *)&p->teamId;
        uint32_t key = (((uint32_t)b[0] << 8) | b[1]) * 16u +
                       (unsigned)p->playerOrdinal + 1u;
        unsigned h = (key * 2654435761u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        while (keys[h] && keys[h] != key)
            h = (h + 1u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        if (keys[h] && latest[h] != i &&
            memcmp(&p->player, &_careerForeignMarketPlayers[latest[h]].player,
                   sizeof(p->player))) {
            p->player = _careerForeignMarketPlayers[latest[h]].player;
            ++repaired;
        }
    }
    if (duplicates || repaired)
        SWOS_AI_LOG("[SWOS AI OVERRIDE] active_duplicates=%u "
                    "active_copies_fixed=%u pending_entries_untouched=%u\n",
                    duplicates, repaired, pending);
    free(keys);
    free(latest);
}

/* Capacity maintenance for long careers.  The active prefix holds changes
 * that have already taken effect; the pending suffix contains scheduled
 * changes whose order and timing must remain intact.  Remove ONLY older
 * active copies when an identical later active record for that same team/
 * physical slot exists.  Never deduplicate the pending suffix or different
 * player payloads, and never touch permanent identity mappings (which use
 * team/slot keys rather than positions in the override array).
 *
 * ReconcileOverrides() runs first so legitimate historical copies with the
 * same key are normalised.  A changed player in two records is nevertheless
 * kept here as a further safeguard against altered ordering semantics. */
static void SWOS_AI_P3CompactActiveOverrides(void)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    const unsigned total = _numForeignMarketPlayers2;
    const unsigned pending = _pendingForeignPlayerCount;
    if (pending > total || total > SWOS_AI_CAREER_POOL_CAPACITY) {
        SWOS_AI_LOG("[SWOS AI POOL] COMPACT_ABORT total=%u pending=%u cap=%u\n",
                    total, pending, (unsigned)SWOS_AI_CAREER_POOL_CAPACITY);
        return;
    }
    const unsigned activeEnd = total - pending;
    if (!activeEnd) return;

    /* Store the LAST active record index per team/slot, by design. */
    uint32_t *keys = calloc(SWOS_AI_OVERRIDE_HASH, sizeof(*keys));
    uint32_t *latest = calloc(SWOS_AI_OVERRIDE_HASH, sizeof(*latest));
    if (!keys || !latest) {
        free(keys); free(latest);
        SWOS_AI_LOG("[SWOS AI POOL] COMPACT_ABORT allocation_failed\n");
        return;
    }
    for (unsigned i = 0; i < activeEnd; ++i) {
        const MarketPlayer *entry = &_careerForeignMarketPlayers[i];
        if (entry->playerOrdinal < 0 || entry->playerOrdinal >= 16) continue;
        const uint8_t *b = (const uint8_t *)&entry->teamId;
        uint32_t key = ((((uint32_t)b[0] << 8) | b[1]) * 16u) +
                       (unsigned)entry->playerOrdinal + 1u;
        unsigned h = (key * 2654435761u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        while (keys[h] && keys[h] != key)
            h = (h + 1u) & (SWOS_AI_OVERRIDE_HASH - 1u);
        keys[h] = key;
        latest[h] = i;
    }

    /* Compact the active prefix in place, preserving its relative order.
     * Every copy of the pending suffix moves intact, including phase bytes. */
    unsigned dst = 0, removed = 0;
    for (unsigned i = 0; i < activeEnd; ++i) {
        const MarketPlayer *entry = &_careerForeignMarketPlayers[i];
        int redundant = 0;
        if (entry->playerOrdinal >= 0 && entry->playerOrdinal < 16) {
            const uint8_t *b = (const uint8_t *)&entry->teamId;
            uint32_t key = ((((uint32_t)b[0] << 8) | b[1]) * 16u) +
                           (unsigned)entry->playerOrdinal + 1u;
            unsigned h = (key * 2654435761u) & (SWOS_AI_OVERRIDE_HASH - 1u);
            while (keys[h] && keys[h] != key)
                h = (h + 1u) & (SWOS_AI_OVERRIDE_HASH - 1u);
            if (keys[h] && latest[h] != i &&
                memcmp(&entry->player,
                       &_careerForeignMarketPlayers[latest[h]].player,
                       sizeof(entry->player)) == 0)
                redundant = 1;
        }
        if (redundant) { ++removed; continue; }
        if (dst != i) _careerForeignMarketPlayers[dst] = *entry;
        ++dst;
    }
    if (removed) {
        memmove(&_careerForeignMarketPlayers[dst],
                &_careerForeignMarketPlayers[activeEnd],
                pending * sizeof(MarketPlayer));
        _numForeignMarketPlayers2 = (uint16_t)(total - removed);
        SWOS_AI_LOG("[SWOS AI POOL] COMPACT active_before=%u active_after=%u "
                    "removed=%u pending_preserved=%u total=%u free=%u\n",
                    activeEnd, dst, removed, pending,
                    (unsigned)_numForeignMarketPlayers2,
                    (unsigned)(SWOS_AI_CAREER_POOL_CAPACITY - _numForeignMarketPlayers2));
    }
    if (_numForeignMarketPlayers2 >= SWOS_AI_CAREER_POOL_CAPACITY * 4u / 5u)
        SWOS_AI_LOG("[SWOS AI POOL] CAPACITY_WARNING used=%u capacity=%u "
                    "pending=%u free=%u\n",
                    (unsigned)_numForeignMarketPlayers2,
                    (unsigned)SWOS_AI_CAREER_POOL_CAPACITY,
                    (unsigned)_pendingForeignPlayerCount,
                    (unsigned)(SWOS_AI_CAREER_POOL_CAPACITY - _numForeignMarketPlayers2));
    free(keys);
    free(latest);
}

/* Diagnostic source-location journal. This records events, NOT permanent
 * player identities. The runtime identity registry must not be enabled from
 * this journal alone. Stored in CAR v4 for subsequent replay/auditing. */
typedef struct SWOS_AI_IdentityJournalEvent {
    uint8_t file_a, team_a, slot_a, file_b, team_b, slot_b;
} SWOS_AI_IdentityJournalEvent;
static SWOS_AI_IdentityJournalEvent *s_identity_journal;
static unsigned s_identity_journal_count;
static unsigned s_identity_journal_capacity;
static unsigned s_identity_journal_dropped;
void SWOS_AI_IdentityJournalClear(void)
{
    s_identity_journal_count = s_identity_journal_dropped = 0;
}
unsigned SWOS_AI_IdentityJournalCount(void) { return s_identity_journal_count; }
unsigned SWOS_AI_IdentityJournalDropped(void) { return s_identity_journal_dropped; }
int SWOS_AI_IdentityJournalGet(unsigned i, unsigned out[6])
{
    if (!out || i >= s_identity_journal_count) return 0;
    const SWOS_AI_IdentityJournalEvent *e = &s_identity_journal[i];
    out[0] = e->file_a; out[1] = e->team_a; out[2] = e->slot_a;
    out[3] = e->file_b; out[4] = e->team_b; out[5] = e->slot_b;
    return 1;
}
int SWOS_AI_IdentityJournalAdd(unsigned fa, unsigned ta, unsigned sa,
                               unsigned fb, unsigned tb, unsigned sb)
{
    if (fa > 255 || ta > 255 || sa >= 16 ||
        fb > 255 || tb > 255 || sb >= 16 ||
        (fa == fb && ta == tb && sa == sb)) return 0;
    if (s_identity_journal_count == s_identity_journal_capacity) {
        unsigned new_capacity = s_identity_journal_capacity ?
            s_identity_journal_capacity * 2u : 4096u;
        if (new_capacity <= s_identity_journal_capacity ||
            (size_t)new_capacity > SIZE_MAX / sizeof(*s_identity_journal)) {
            ++s_identity_journal_dropped;
            return 0;
        }
        void *new_events = realloc(s_identity_journal,
                                   (size_t)new_capacity * sizeof(*s_identity_journal));
        if (!new_events) {
            ++s_identity_journal_dropped;
            return 0;
        }
        s_identity_journal = new_events;
        s_identity_journal_capacity = new_capacity;
    }
    s_identity_journal[s_identity_journal_count++] =
        (SWOS_AI_IdentityJournalEvent){(uint8_t)fa,(uint8_t)ta,(uint8_t)sa,
                                       (uint8_t)fb,(uint8_t)tb,(uint8_t)sb};
    return 1;
}

/* Reserves both override slots before doing anything so a full pool cannot
 * produce a one-sided exchange. */

/* Step 2U: shadow ID registry indexed by collision-free canonical TEAM location.
 * The seed contains every imported original player. IDs and ages travel together
 * on AI swaps; no changes to the 38-byte SWOS player record.
 * Ordinary transfers and generated players are NOT yet connected, so this
 * registry is intentionally not authoritative for the AGE menu. */
typedef struct { uint32_t id; uint8_t age; uint8_t source; } SWOS_TrackedIdentity;
static SWOS_TrackedIdentity *s_identity_records;
static unsigned s_identity_next_id = 1;
static unsigned s_identity_faults;
/* Step 4F: retired permanent IDs persist independently of roster slots.
 * All values are unsigned so this record is portable across CAR v4 builds. */
typedef struct { uint32_t id,season,age,club,nationality; } SWOS_RetiredId;
static SWOS_RetiredId *s_retired_ids;
static unsigned s_retired_count,s_retired_capacity;
/* A retired ID must never remain claimed by an active manual roster slot.
 * Defined after the manual identity array, but used by retirement restore. */
static unsigned SWOS_ManualIdentityClearRetired(unsigned id, const char *phase);
unsigned SWOS_RetiredIdentityCount(void){return s_retired_count;}
unsigned SWOS_RetiredIdentityGet(unsigned i,unsigned out[5]){
    if(!out||i>=s_retired_count)return 0;
    const SWOS_RetiredId *p=&s_retired_ids[i];
    out[0]=p->id;out[1]=p->season;out[2]=p->age;
    out[3]=p->club;out[4]=p->nationality;
    return 1;
}
unsigned SWOS_RetiredIdentityContains(unsigned id){
    for(unsigned i=0;i<s_retired_count;i++)if(s_retired_ids[i].id==id)return 1;
    return 0;
}
static unsigned SWOS_RetiredIdentityAppend(unsigned id,unsigned season,unsigned age,
                                            unsigned club,unsigned nationality){
    if(!id||id>=s_identity_next_id||age<15||age>255||club>65535||
       nationality>255||SWOS_RetiredIdentityContains(id))return 0;
    if(s_retired_count==s_retired_capacity){
        unsigned cap=s_retired_capacity?s_retired_capacity*2u:64u;
        if(cap>s_identity_next_id||cap< s_retired_capacity)cap=s_identity_next_id;
        if(cap<=s_retired_count)return 0;
        void *tmp=realloc(s_retired_ids,(size_t)cap*sizeof(*s_retired_ids));
        if(!tmp)return 0;
        s_retired_ids=tmp;s_retired_capacity=cap;
    }
    s_retired_ids[s_retired_count++]=(SWOS_RetiredId){id,season,age,club,nationality};
    /* Retirements must atomically invalidate any obsolete manual claim.
     * Canonical shadows are deliberately retained. */
    SWOS_ManualIdentityClearRetired(id,"retirement");
    return 1;
}
unsigned SWOS_RetiredIdentityRestore(const unsigned *rows,unsigned count){
    if((count&&!rows)||count>100000u)return 0;
    SWOS_RetiredId *items=count?malloc((size_t)count*sizeof(*items)):NULL;
    if(count&&!items)return 0;
    unsigned valid=1;
    for(unsigned i=0;i<count;i++){
        unsigned id=rows[i*5],season=rows[i*5+1],age=rows[i*5+2],
                 club=rows[i*5+3],nat=rows[i*5+4];
        if(!id||id>=s_identity_next_id||age<15||age>255||club>65535||nat>255){valid=0;break;}
        for(unsigned j=0;j<i;j++)if(items[j].id==id){valid=0;break;}
        if(!valid)break;
        /* A retired ID must not simultaneously occupy an active generated
         * or manual-transfer identity slot. The canonical registry can
         * contain historical shadows, so it is deliberately excluded. */
        for(unsigned k=0;k<SWOS_GeneratedIdentityCount();k++){
            unsigned g[4];
            if(SWOS_GeneratedIdentityGet(k,g)&&g[2]==id){valid=0;break;}
        }
        /* Previously saved careers may contain obsolete manual claims for
         * retirees. Validate the retirement ledger here, then tombstone the
         * conflicting claims after the entire ledger is accepted. This
         * preserves the historical retirement and every actual squad record. */
        if(!valid)break;
        items[i]=(SWOS_RetiredId){id,season,age,club,nat};
    }
    if(!valid){free(items);return 0;}
    free(s_retired_ids);s_retired_ids=items;s_retired_count=s_retired_capacity=count;
    unsigned cleared=0;
    for(unsigned i=0;i<count;i++)
        cleared+=SWOS_ManualIdentityClearRetired(items[i].id,"load");
    if(cleared)swos_age_log("RETIRE_MANUAL_LOAD_RECOVERY",
                            "retired=%u cleared_claims=%u",count,cleared);
    return 1;
}

/* TEAM record order and the in-game team ordinal are different identifiers.
 * Only country snapshots that were actually decoded may register a mapping.
 * A missing mapping must NEVER be guessed from team ordinal. */
static int SWOS_IdentityFind(uint32_t key);
static void SWOS_IdentityResolveClub(unsigned club);
static void SWOS_IdentityEnsureExtension(unsigned club,unsigned file,unsigned record);
static void SWOS_IdentityBootstrapExpandedTeams(void);
unsigned SWOS_ManualIdentityIsTouched(unsigned,unsigned);
unsigned SWOS_ManualIdentityAge(unsigned,unsigned,unsigned*,unsigned*);
static uint16_t s_identity_club_sources[65536];
void SWOS_IdentityRegisterClub(unsigned club,unsigned file,unsigned record)
{
    if(club>65535u || file>255u || record>255u)return;
    uint16_t value=(uint16_t)(((file<<8)|record)+1u);
    if(value==0u)return;
    if(s_identity_club_sources[club] && s_identity_club_sources[club]!=value){
        s_identity_club_sources[club]=0;
        swos_age_log("IDENTITY_CLUB_CONFLICT","club=%04X file=%u record=%u",club,file,record);
        return;
    }
    s_identity_club_sources[club]=value;
    SWOS_IdentityEnsureExtension(club,file,record);
}
/* A manual transfer changes live squad records without moving original IDs.
 * Never display a canonical-slot age for an affected club in this session. */
void SWOS_IdentityInvalidateClub(unsigned club)
{
    if(club>65535u)return;
    if(s_identity_club_sources[club]){
        s_identity_club_sources[club]=0;
        swos_age_log("IDENTITY_CLUB_INVALIDATE","club=%04X reason=manual_transfer",club);
    }
}
#define SWOS_GENERATED_ID_MAX_SLOT 31u

unsigned SWOS_IdentityAgeForClub(unsigned club,unsigned slot,unsigned *age,unsigned *id)
{
    if(!age||!id||club>65535u||slot>SWOS_GENERATED_ID_MAX_SLOT)return 0;
    /* Generated and manual identities cover the full squad. Canonical
     * TEAM records only cover the original 16 physical slots. */
    { unsigned gen[4];
      extern unsigned SWOS_GeneratedIdentityCount(void);
      extern unsigned SWOS_GeneratedIdentityGet(unsigned,unsigned [4]);
      for(unsigned gi=0;gi<SWOS_GeneratedIdentityCount();gi++)
          if(SWOS_GeneratedIdentityGet(gi,gen)&&gen[0]==club&&gen[1]==slot){
              *age=gen[3];*id=gen[2];return 1;
          }
    }
    if(SWOS_ManualIdentityIsTouched(club,slot))return SWOS_ManualIdentityAge(club,slot,age,id);
    if(slot>=16u||!s_identity_records)return 0;
    unsigned v=s_identity_club_sources[club];
    /* The club-to-TEAM source map is runtime-only, not persisted in .CAR.
     * After loading a career, restore the managed club mapping on demand
     * before reading its canonical player ages. Individual manual-transfer
     * slots were checked above and remain authoritative. */
    if(!v && club==(unsigned)_selTeamNumber){
        SWOS_IdentityResolveClub(club);
        v=s_identity_club_sources[club];
    }
    /* Human-team compatibility for legacy saves whose runtime TEAM source
     * mapping is unavailable. Canonical retirement replacements are stored
     * in the persisted identity registry with source=0 and a newly allocated
     * ID. For clubs whose TEAM record index equals their team ordinal, this
     * precise key recovers a replacement without guessing an original
     * footballer's age. Never apply it to AI clubs, transferred slots, or
     * ordinary seeded identities. */
    if(club==(unsigned)_selTeamNumber) {
        unsigned fallbackFile=(club>>8)&255u;
        unsigned fallbackRecord=club&255u;
        int fallbackIndex=SWOS_IdentityFind(
            ((uint32_t)fallbackFile<<12)|
            ((uint32_t)fallbackRecord<<4)|slot);
        if(fallbackIndex>=0 &&
           s_identity_records[fallbackIndex].id>SWOS_IDENTITY_SEED_COUNT &&
           s_identity_records[fallbackIndex].source==0 &&
           s_identity_records[fallbackIndex].age>=15u &&
           s_identity_records[fallbackIndex].age<=60u) {
            /* Honour the normal mapping if it points to the same canonical
             * record. A different mapped record wins below: ordinal need
             * not equal the original TEAM file index for every club. */
            if(!v || (((v-1u)>>8)==fallbackFile &&
                      ((v-1u)&255u)==fallbackRecord)) {
                *age=s_identity_records[fallbackIndex].age;
                *id=s_identity_records[fallbackIndex].id;
                swos_age_log("MANAGED_AGE_REPLACEMENT_RECOVER",
                    "club=%04X slot=%u id=%u age=%u source_map=%u",
                    club,slot,*id,*age,v);
                return 1;
            }
        }
    }
    if(!v)return 0;
    unsigned file=(v-1u)>>8,record=(v-1u)&255u;
    int index=SWOS_IdentityFind(((uint32_t)file<<12)|((uint32_t)record<<4)|slot);
    if(index<0)return 0;
    *age=s_identity_records[index].age;
    *id=s_identity_records[index].id;
    return *id!=0u;
}

/* Step 3D: manual roster-copy shadow records. Zero IDs are tombstones.
 * The original fixed-size AI registry is intentionally unchanged because its
 * save format requires one original ID per canonical seed slot. These
 * overrides follow original footballers into extended 32-slot rosters. */
typedef struct { unsigned club,slot,id,age,source; } SWOS_ManualIdentity;
static SWOS_ManualIdentity *s_manual_ids;
static unsigned s_manual_count,s_manual_capacity;
static int SWOS_ManualFind(unsigned club,unsigned slot){
    for(unsigned i=0;i<s_manual_count;i++)if(s_manual_ids[i].club==club&&s_manual_ids[i].slot==slot)return (int)i;
    return -1;
}
static int SWOS_ManualPut(unsigned club,unsigned slot,unsigned id,unsigned age,unsigned source){
    int at=SWOS_ManualFind(club,slot);
    if(at>=0){s_manual_ids[at]=(SWOS_ManualIdentity){club,slot,id,age,source};return 1;}
    if(s_manual_count==s_manual_capacity){
        unsigned cap=s_manual_capacity?s_manual_capacity*2u:32u;
        if(cap>65536u||cap<s_manual_capacity)return 0;
        void *mem=realloc(s_manual_ids,(size_t)cap*sizeof(*s_manual_ids));
        if(!mem)return 0;s_manual_ids=mem;s_manual_capacity=cap;
    }
    s_manual_ids[s_manual_count++]=(SWOS_ManualIdentity){club,slot,id,age,source};return 1;
}
/* Expanded TEAM files can have real original players which were added after
 * the 1996 canonical identity seed was generated. The seed's key ordering
 * and original ID range MUST stay unchanged, otherwise existing CAR files
 * fail schema-4 validation. Store missing source slots as persistent manual
 * identities, using the existing CAR manual registry instead.
 *
 * Do not overwrite transferred/manual slots, tombstones or generated youths.
 * In old careers the original age is unknowable without a matching age CSV;
 * use a deterministic baseline 18..30 + seasons completed. In a new career
 * this establishes a stable initial age and all subsequent seasons increment
 * it via SWOS_IdentityAdvanceSeason(). */
unsigned SWOS_GeneratedIdentityCount(void);
unsigned SWOS_GeneratedIdentityGet(unsigned index,unsigned values[4]);
static void SWOS_IdentityEnsureExtension(unsigned club,unsigned file,unsigned record)
{
    if (!s_identity_records || file > 79u || club > 65535u) return;
    unsigned added=0,covered=0,conflicts=0;
    for (unsigned slot=0;slot<16u;slot++) {
        uint32_t key=((uint32_t)file<<12)|((uint32_t)record<<4)|slot;
        if (SWOS_IdentityFind(key)>=0) { covered++; continue; }
        /* Currently TEAM.047 is the only documented extension. Skip other
         * files until their roster layout and canonical mapping are audited. */
        if (file!=47u) continue;
        if (SWOS_ManualFind(club,slot)>=0) continue;
        /* Existing generated replacements already own their physical slot. */
        unsigned occupied=0;
        unsigned generated[4];
        for (unsigned g=0;g<SWOS_GeneratedIdentityCount();g++)
            if (SWOS_GeneratedIdentityGet(g,generated) &&
                generated[0]==club && generated[1]==slot) {
                occupied=1;break;
            }
        if (occupied) { conflicts++;continue; }
        if (s_identity_next_id>=4000000u) { conflicts++;continue; }
        unsigned baseline=18u+((file*131u+record*37u+slot*17u)%13u);
        unsigned age=baseline+(unsigned)_careerSeasonsCompleted;
        if (age>60u) age=60u;
        unsigned id=s_identity_next_id;
        if (!SWOS_ManualPut(club,slot,id,age,0u)) {
            conflicts++;continue;
        }
        s_identity_next_id++;
        added++;
    }
    if (added || conflicts)
        swos_age_log("IDENTITY_EXTENSION",
            "club=%04X file=%u record=%u seeded=%u added=%u skipped=%u next_id=%u",
            club,file,record,covered,added,conflicts,s_identity_next_id);
}
/* Human purchases create a new AI trialist in the vacated physical slot.
 * The purchased footballer's ID has already moved to the human roster.
 * Replace the source slot's tombstone with a fresh, persistent 17-year-old ID. */
unsigned SWOS_ManualIdentityCreateYouth(unsigned club,unsigned slot){
    if(club>65535u||slot>31u||!SWOS_IdentitySeed()||
       s_identity_next_id>=4000000u)return 0;
    unsigned id=s_identity_next_id;
    if(!SWOS_ManualPut(club,slot,id,17u,0u))return 0;
    ++s_identity_next_id;
    swos_age_log("PURCHASE_AI_YOUTH_ID", "club=%04X slot=%u id=%u age=17",
                 club,slot,id);
    return id;
}
/* Tombstone only the stale identity metadata, not the footballer or its
 * attributes. Keep the slot marked 'touched' so the canonical age fallback
 * cannot resurrect the retired identity. No player ID is ever reused here. */
static unsigned SWOS_ManualIdentityClearRetired(unsigned id,const char *phase){
    unsigned cleared=0;
    if(!id)return 0;
    for(unsigned i=0;i<s_manual_count;i++){
        SWOS_ManualIdentity *m=&s_manual_ids[i];
        if(m->id!=id)continue;
        swos_age_log("RETIRE_MANUAL_TOMBSTONE",
            "phase=%s id=%u club=%04X slot=%u previous_age=%u",
            phase?phase:"unknown",id,m->club,m->slot,m->age);
        m->id=0;m->age=0;m->source=0;
        cleared++;
    }
    return cleared;
}

/* Step 4Q: the JSON loader rejects the same nonzero manual ID in two slots.
 * Old AI exchanges and roster-copy paths may leave a stale claim behind.
 * Only tombstone the obsolete identity claim, never touch the player record.
 * Called before writing the manual registry; the save is then reloadable.
 * Keep the first active claim, as in the successfully recovered BUDALE save.
 * Diagnose all changes so an incorrect underlying transfer remains visible. */
unsigned SWOS_ManualIdentityPrepareSave(void){
    unsigned retiredClaims=0;
    for(unsigned i=0;i<s_manual_count;i++)
        if(s_manual_ids[i].id && SWOS_RetiredIdentityContains(s_manual_ids[i].id))
            retiredClaims+=SWOS_ManualIdentityClearRetired(s_manual_ids[i].id,"save");
    unsigned duplicates=0;
    for(unsigned i=0;i<s_manual_count;i++){
        unsigned id=s_manual_ids[i].id;
        if(!id)continue;
        for(unsigned j=i+1;j<s_manual_count;j++){
            if(s_manual_ids[j].id!=id)continue;
            swos_age_log("MANUAL_ID_SAVE_DEDUP",
                "id=%u keep=%04X:%u drop=%04X:%u age=%u",
                id,s_manual_ids[i].club,s_manual_ids[i].slot,
                s_manual_ids[j].club,s_manual_ids[j].slot,
                s_manual_ids[j].age);
            s_manual_ids[j].id=0;
            s_manual_ids[j].age=0;
            s_manual_ids[j].source=0;
            duplicates++;
        }
    }
    swos_age_log("MANUAL_ID_SAVE_VALIDATE",
        "rows=%u removed_duplicate_claims=%u removed_retired_claims=%u",
        s_manual_count,duplicates,retiredClaims);
    return duplicates + retiredClaims;
}
unsigned SWOS_ManualIdentityCount(void){return s_manual_count;}
unsigned SWOS_ManualIdentityGet(unsigned idx,unsigned out[5]){
    if(idx>=s_manual_count||!out)return 0;
    SWOS_ManualIdentity x=s_manual_ids[idx];
    out[0]=x.club;out[1]=x.slot;out[2]=x.id;out[3]=x.age;out[4]=x.source;
    return 1;
}
unsigned SWOS_ManualIdentityRestore(const unsigned *rows,unsigned count){
    if(count>65536u||(!rows&&count))return 0;
    SWOS_ManualIdentity *items=count?malloc((size_t)count*sizeof(*items)):NULL;
    if(count&&!items)return 0;
    for(unsigned i=0;i<count;i++){
        unsigned club=rows[5*i],slot=rows[5*i+1],id=rows[5*i+2],age=rows[5*i+3],source=rows[5*i+4];
        if(club>65535u||slot>31u||(id && id>=s_identity_next_id)||age>255u||source>2u){free(items);return 0;}
        for(unsigned j=0;j<i;j++)if((items[j].club==club&&items[j].slot==slot)||
           (id&&items[j].id==id)){free(items);return 0;}
        items[i]=(SWOS_ManualIdentity){club,slot,id,age,source};
    }
    free(s_manual_ids);s_manual_ids=items;s_manual_count=s_manual_capacity=count;
    swos_age_log("MANUAL_ID_LOAD","count=%u valid=1",count);return 1;
}
/* Manual slots override canonical lookup, including tombstones. */
unsigned SWOS_ManualIdentityAge(unsigned club,unsigned slot,unsigned *age,unsigned *id){
    int i=SWOS_ManualFind(club,slot);
    if(i<0||!s_manual_ids[i].id||!age||!id)return 0;
    *age=s_manual_ids[i].age;*id=s_manual_ids[i].id;return 1;
}
unsigned SWOS_ManualIdentityIsTouched(unsigned club,unsigned slot){
    return SWOS_ManualFind(club,slot)>=0;
}
/* Step 4C.3: replace a verified manually tracked footballer in-place.
 * No generated-registry entry is made: manual identity is authoritative for
 * the transferred player's physical slot, including reserve slots 16..31.
 * Assign an ID only after the caller has prepared a valid PlayerFile. */
static unsigned SWOS_ManualIdentityReplace(unsigned club,unsigned slot,
                                            unsigned age,unsigned *retired){
    if(retired)*retired=0;
    int index=SWOS_ManualFind(club,slot);
    if(index<0||!s_manual_ids[index].id||age>255u||
       s_identity_next_id>=4000000u)return 0;
    unsigned old=s_manual_ids[index].id;
    unsigned fresh=s_identity_next_id++;
    s_manual_ids[index].id=fresh;
    s_manual_ids[index].age=age;
    s_manual_ids[index].source=0;
    if(retired)*retired=old;
    swos_age_log("MANUAL_ID_REPLACE", "club=%04X slot=%u retired_id=%u new_id=%u age=%u",
                 club,slot,old,fresh,age);
    return fresh;
}
/* The caller must supply a verified original identity; never infer one from
 * squad ordinal. Missing canonical mappings fail safely. */
unsigned SWOS_ManualIdentityCopy(unsigned srcClub,unsigned srcSlot,unsigned dstClub,unsigned dstSlot){
    if(srcClub>65535u||dstClub>65535u||srcSlot>31u||dstSlot>31u||
       (srcClub==dstClub&&srcSlot==dstSlot))return 0;
    unsigned age=0,id=0,source=0;
    if(srcSlot<16u && !s_identity_club_sources[srcClub])SWOS_IdentityResolveClub(srcClub);
    int old=SWOS_ManualFind(srcClub,srcSlot);
    if(old>=0){
        SWOS_ManualIdentity x=s_manual_ids[old];id=x.id;age=x.age;source=x.source;
    }else if(srcSlot<16u && s_identity_club_sources[srcClub] && s_identity_records){
        unsigned v=s_identity_club_sources[srcClub]-1u;
        int idx=SWOS_IdentityFind(((uint32_t)(v>>8)<<12)|((uint32_t)(v&255u)<<4)|srcSlot);
        if(idx>=0){id=s_identity_records[idx].id;age=s_identity_records[idx].age;source=s_identity_records[idx].source;}
    }
    if(!id){swos_age_log("MANUAL_ID_UNRESOLVED","from=%04X:%u to=%04X:%u",srcClub,srcSlot,dstClub,dstSlot);return 0;}
    /* First reserve both locations, avoiding a partial transfer on allocation failure. */
    if(!SWOS_ManualPut(dstClub,dstSlot,id,age,source))return 0;
    if(!SWOS_ManualPut(srcClub,srcSlot,0,0,0))return 0;
    /* Step 3F: an original ID can have only one active manual location.
     * A second transfer of that same player must supersede the previous
     * location, even if an old transfer record survived in the CAR file. */
    unsigned superseded=0;
    for(unsigned i=0;i<s_manual_count;i++){
        if(s_manual_ids[i].id==id &&
           !(s_manual_ids[i].club==srcClub&&s_manual_ids[i].slot==srcSlot) &&
           !(s_manual_ids[i].club==dstClub&&s_manual_ids[i].slot==dstSlot)){
            s_manual_ids[i].id=0;
            s_manual_ids[i].age=0;
            s_manual_ids[i].source=0;
            superseded++;
        }
    }
    if(superseded)swos_age_log("MANUAL_ID_RECONCILE","id=%u superseded=%u destination=%04X:%u",id,superseded,dstClub,dstSlot);
    swos_age_log("MANUAL_ID_MOVE","from=%04X:%u to=%04X:%u id=%u age=%u",srcClub,srcSlot,dstClub,dstSlot,id,age);
    return id;
}

/* Step 3F.2: AI swaps must carry an existing manual override with
 * the physical player. For an untouched opposite slot the canonical identity
 * is resolved before the canonical swap, never reconstructed afterwards.
 * If either side is a tombstone or cannot be mapped, mark both locations
 * unknown so the AGE menu falls back rather than showing an unrelated age. */
unsigned SWOS_ManualIdentityAISwap(unsigned clubA,unsigned slotA,
                                    unsigned clubB,unsigned slotB)
{
    if(clubA>65535u||clubB>65535u||slotA>=16u||slotB>=16u||
       (clubA==clubB&&slotA==slotB))return 0;
    int ia=SWOS_ManualFind(clubA,slotA),ib=SWOS_ManualFind(clubB,slotB);
    if(ia<0&&ib<0)return 1;
    SWOS_ManualIdentity left={clubA,slotA,0,0,0};
    SWOS_ManualIdentity right={clubB,slotB,0,0,0};
    if(ia>=0)left=s_manual_ids[ia];
    else{
        unsigned age=0,id=0;
        if(SWOS_IdentityAgeForClub(clubA,slotA,&age,&id)){
            left.id=id;left.age=age;
        }
    }
    if(ib>=0)right=s_manual_ids[ib];
    else{
        unsigned age=0,id=0;
        if(SWOS_IdentityAgeForClub(clubB,slotB,&age,&id)){
            right.id=id;right.age=age;
        }
    }
    /* Allocate missing keys up front; do not commit an incomplete exchange. */
    if(ia<0&&!SWOS_ManualPut(clubA,slotA,0,0,0))return 0;
    if(ib<0&&!SWOS_ManualPut(clubB,slotB,0,0,0))return 0;
    ia=SWOS_ManualFind(clubA,slotA);ib=SWOS_ManualFind(clubB,slotB);
    if(ia<0||ib<0)return 0;
    if(!left.id||!right.id){
        s_manual_ids[ia].id=s_manual_ids[ib].id=0;
        s_manual_ids[ia].age=s_manual_ids[ib].age=0;
        s_manual_ids[ia].source=s_manual_ids[ib].source=0;
        swos_age_log("MANUAL_ID_AI_UNKNOWN","club_a=%04X slot_a=%u club_b=%04X slot_b=%u reason=unresolved",clubA,slotA,clubB,slotB);
        return 1;
    }
    s_manual_ids[ia].id=right.id;s_manual_ids[ia].age=right.age;s_manual_ids[ia].source=right.source;
    s_manual_ids[ib].id=left.id;s_manual_ids[ib].age=left.age;s_manual_ids[ib].source=left.source;
    /* The two destination records are authoritative after this exchange.
     * A prior transfer can leave the same ID at an unrelated manual slot.
     * Turn that old claim into a tombstone before a future save/load. */
    for(unsigned i=0;i<s_manual_count;i++){
        if((int)i==ia||(int)i==ib||!s_manual_ids[i].id)continue;
        unsigned moved=s_manual_ids[i].id;
        if(moved==s_manual_ids[ia].id || moved==s_manual_ids[ib].id){
            swos_age_log("MANUAL_ID_AI_STALE",
                "id=%u stale=%04X:%u moved_to_a=%04X:%u moved_to_b=%04X:%u",
                moved,s_manual_ids[i].club,s_manual_ids[i].slot,
                clubA,slotA,clubB,slotB);
            s_manual_ids[i].id=0;
            s_manual_ids[i].age=0;
            s_manual_ids[i].source=0;
        }
    }
    swos_age_log("MANUAL_ID_AI_SWAP","club_a=%04X slot_a=%u id_a=%u club_b=%04X slot_b=%u id_b=%u",clubA,slotA,right.id,clubB,slotB,left.id);
    return 1;
}

/* Independently tracked generated players. A team/slot key denotes the
 * generation location, not yet an authoritative live roster identity. */
typedef struct { unsigned team,slot,id,age; } SWOS_GeneratedIdentity;
static SWOS_GeneratedIdentity *s_generated;
static unsigned s_generated_count,s_generated_capacity;
unsigned SWOS_GeneratedIdentityCount(void){return s_generated_count;}
unsigned SWOS_GeneratedIdentityGet(unsigned index,unsigned values[4]){
    if(index>=s_generated_count||!values)return 0;
    values[0]=s_generated[index].team;values[1]=s_generated[index].slot;
    values[2]=s_generated[index].id;values[3]=s_generated[index].age;return 1;
}
/* Generated RES/TRIAL footballers occupy physical squad slots above 15.
 * The original AI exchange registry handles only the first 16 positions;
 * generated identities must accept all 32 physical player slots. */
unsigned SWOS_GeneratedIdentityCreate(unsigned team,unsigned slot,unsigned age){
    if(team>65535||slot>SWOS_GENERATED_ID_MAX_SLOT||age>255||!SWOS_IdentitySeed()){
        swos_age_log("GENERATED_ID_REJECT","team=%04X slot=%u age=%u reason=invalid_slot_or_seed",team,slot,age);
        return 0;
    }
    /* Creation in a reused slot retires the previous generated identity. */
    for(unsigned i=0;i<s_generated_count;i++)if(s_generated[i].team==team&&s_generated[i].slot==slot){
        unsigned old=s_generated[i].id;
        s_generated[i].id=s_identity_next_id++;s_generated[i].age=age;
        swos_age_log("GENERATED_ID_REPLACE","team=%04X slot=%u old_id=%u id=%u age=%u",team,slot,old,s_generated[i].id,age);
        return s_generated[i].id;
    }
    if(s_generated_count==s_generated_capacity){
        unsigned cap=s_generated_capacity?s_generated_capacity*2:32;
        if(cap>65536||cap<s_generated_capacity)return 0;
        void *p=realloc(s_generated,(size_t)cap*sizeof(*s_generated));
        if(!p)return 0;s_generated=p;s_generated_capacity=cap;
    }
    unsigned id=s_identity_next_id++;
    s_generated[s_generated_count++]=(SWOS_GeneratedIdentity){team,slot,id,age};
    swos_age_log("GENERATED_ID_CREATE","team=%04X slot=%u id=%u age=%u count=%u",team,slot,id,age,s_generated_count);
    return id;
}
/* Identities are keyed by physical team slots, not squad-list ordinals.
 * Move only an existing generated ID. Do not mint a new one here: ordinary
 * purchases may move original players whose canonical records are still
 * tracked by the AI shadow registry. */
unsigned SWOS_GeneratedIdentityRetire(unsigned team,unsigned slot){
    for(unsigned i=0;i<s_generated_count;i++)
        if(s_generated[i].team==team&&s_generated[i].slot==slot){
            unsigned id=s_generated[i].id;
            s_generated[i]=s_generated[--s_generated_count];
            swos_age_log("GENERATED_ID_RETIRE","team=%04X slot=%u id=%u",team,slot,id);
            return id;
        }
    return 0;
}
unsigned SWOS_GeneratedIdentityMove(unsigned from_team,unsigned from_slot,unsigned to_team,unsigned to_slot){
    if(from_team==to_team&&from_slot==to_slot)return 0;
    unsigned found=s_generated_count;
    for(unsigned i=0;i<s_generated_count;i++)
        if(s_generated[i].team==from_team&&s_generated[i].slot==from_slot){found=i;break;}
    /* The physical destination has been overwritten even when the incoming
     * footballer is an original seed player, so retire an old generated ID. */
    unsigned retired=SWOS_GeneratedIdentityRetire(to_team,to_slot);
    found=s_generated_count;
    for(unsigned i=0;i<s_generated_count;i++)
        if(s_generated[i].team==from_team&&s_generated[i].slot==from_slot){found=i;break;}
    if(found<s_generated_count&&s_generated[found].team==from_team&&s_generated[found].slot==from_slot){
        unsigned id=s_generated[found].id;
        s_generated[found].team=to_team;s_generated[found].slot=to_slot;
        swos_age_log("GENERATED_ID_MOVE","from=%04X:%u to=%04X:%u id=%u retired=%u",from_team,from_slot,to_team,to_slot,id,retired);
        return id;
    }
    swos_age_log("GENERATED_ID_MOVE_ORIGINAL","from=%04X:%u to=%04X:%u retired=%u",from_team,from_slot,to_team,to_slot,retired);
    return 0;
}
/* AI exchanges swap physical players. Move generated identities with them,
 * including the case where only one side is generated. */
static void SWOS_GeneratedIdentityAISwap(unsigned teamA,unsigned slotA,
                                         unsigned teamB,unsigned slotB){
    int a=-1,b=-1;
    for(unsigned i=0;i<s_generated_count;i++){
        if(s_generated[i].team==teamA&&s_generated[i].slot==slotA)a=(int)i;
        if(s_generated[i].team==teamB&&s_generated[i].slot==slotB)b=(int)i;
    }
    if(a>=0){s_generated[a].team=teamB;s_generated[a].slot=slotB;}
    if(b>=0){s_generated[b].team=teamA;s_generated[b].slot=slotA;}
    if(a>=0||b>=0)swos_age_log("GENERATED_ID_AI_SWAP",
        "team_a=%04X slot_a=%u team_b=%04X slot_b=%u generated_a=%u generated_b=%u",
        teamA,slotA,teamB,slotB,(unsigned)(a>=0),(unsigned)(b>=0));
}
/* Reconcile historical shadow-identity claims before writing a career.
 * The separate registries were introduced at different times and a move can
 * leave an obsolete generated entry or a manual zero-ID placeholder behind.
 * Only identity metadata is changed, never a PlayerFile or its attributes. */
unsigned SWOS_IdentityPrepareSave(void)
{
    unsigned changes=0;
    unsigned highest=SWOS_IDENTITY_SEED_COUNT;
    for(unsigned i=0;i<SWOS_IDENTITY_SEED_COUNT && s_identity_records;i++)
        if(s_identity_records[i].id>highest)highest=s_identity_records[i].id;
    for(unsigned i=0;i<s_manual_count;i++)
        if(s_manual_ids[i].id>highest)highest=s_manual_ids[i].id;
    for(unsigned i=0;i<s_generated_count;i++)
        if(s_generated[i].id>highest)highest=s_generated[i].id;
    for(unsigned i=0;i<s_retired_count;i++)
        if(s_retired_ids[i].id>highest)highest=s_retired_ids[i].id;
    if(s_identity_next_id<=highest)s_identity_next_id=highest+1u;
    if(s_identity_next_id>4000000u)return 0;

    /* An identity in the immutable retirement history cannot also be a
     * live generated identity. This is the generated counterpart of the
     * manual tombstone repair. Remove only the stale metadata claim and
     * retain the roster and historical retirement entry unchanged. */
    for (unsigned i=0; i<s_generated_count;) {
        if (SWOS_RetiredIdentityContains(s_generated[i].id)) {
            swos_age_log("IDENTITY_SAVE_RETIRED_GENERATED",
                "club=%04X slot=%u retired_id=%u",
                s_generated[i].team, s_generated[i].slot, s_generated[i].id);
            memmove(&s_generated[i], &s_generated[i+1],
                (size_t)(s_generated_count-i-1)*sizeof(*s_generated));
            --s_generated_count;
            ++changes;
            continue;
        }
        ++i;
    }
    /* A live manual claim wins if the same physical slot also has a
     * generated claim. The latter is a stale snapshot after a transfer. */
    for(unsigned i=0;i<s_generated_count;){
        int mi=SWOS_ManualFind(s_generated[i].team,s_generated[i].slot);
        if(mi>=0 && s_manual_ids[mi].id){
            swos_age_log("IDENTITY_SAVE_STALE_GENERATED",
                "club=%04X slot=%u old_id=%u manual_id=%u",
                s_generated[i].team,s_generated[i].slot,s_generated[i].id,
                s_manual_ids[mi].id);
            memmove(&s_generated[i],&s_generated[i+1],
                (size_t)(s_generated_count-i-1)*sizeof(*s_generated));
            s_generated_count--;changes++;continue;
        }
        i++;
    }
    /* Generated players supersede zero-ID manual tombstones in the same
     * physical slot. Remove the obsolete placeholder, not the player. */
    for(unsigned i=0;i<s_manual_count;){
        unsigned matched=0;
        if(!s_manual_ids[i].id)
            for(unsigned j=0;j<s_generated_count;j++)
                if(s_manual_ids[i].club==s_generated[j].team &&
                   s_manual_ids[i].slot==s_generated[j].slot){matched=1;break;}
        if(matched){
            memmove(&s_manual_ids[i],&s_manual_ids[i+1],
                (size_t)(s_manual_count-i-1)*sizeof(*s_manual_ids));
            s_manual_count--;changes++;continue;
        }
        i++;
    }
    /* Never preserve duplicate generated IDs or an active manual/generated
     * clash. Allocate a new identity while keeping its existing age. */
    for(unsigned i=0;i<s_generated_count;i++){
        unsigned conflict=0;
        for(unsigned j=0;j<i;j++)
            if(s_generated[j].id==s_generated[i].id){conflict=1;break;}
        for(unsigned j=0;j<s_manual_count && !conflict;j++)
            if(s_manual_ids[j].id &&
               s_manual_ids[j].id==s_generated[i].id)conflict=1;
        if(conflict){
            if(s_identity_next_id>=4000000u)return changes;
            unsigned old=s_generated[i].id;
            s_generated[i].id=s_identity_next_id++;
            swos_age_log("IDENTITY_SAVE_REASSIGN",
                "club=%04X slot=%u old_id=%u new_id=%u age=%u",
                s_generated[i].team,s_generated[i].slot,old,
                s_generated[i].id,s_generated[i].age);
            changes++;
        }
    }
    if(changes)swos_age_log("IDENTITY_SAVE_RECONCILE",
        "changes=%u next_id=%u generated=%u manual=%u",
        changes,s_identity_next_id,s_generated_count,s_manual_count);
    return changes;
}

/* Career+ integrity checkpoint. Run after seasonal player moves and before
 * ANY career serialisation. Repair only identity metadata that the existing
 * reconciliation code can resolve deterministically; never edit PlayerFiles,
 * ages, results or historical retirement records. Return false on a remaining
 * problem so a known-good save is not overwritten. */
unsigned SWOS_CareerIntegrityCheck(const char *phase, unsigned season)
{
    const char *where = phase ? phase : "unknown";
    unsigned before_manual = s_manual_count, before_generated = s_generated_count;
    unsigned before_next = s_identity_next_id;
    unsigned repairs = 0, errors = 0;
    unsigned retired_manual = 0, retired_generated = 0;
    unsigned duplicate_manual = 0, duplicate_generated = 0, conflicting_slots = 0;
    unsigned highest = SWOS_IDENTITY_SEED_COUNT;

    /* The identity seed is initialised on entering career mode. Do not
     * destructively guess identities if this routine is called too early. */
    if (!s_identity_records && !SWOS_IdentitySeed()) {
        swos_age_log("INTEGRITY_SKIP", "phase=%s season=%u seed_unavailable=1", where, season);
        return 1;
    }
    SWOS_IdentityBootstrapExpandedTeams();
    repairs += SWOS_IdentityPrepareSave();
    repairs += SWOS_ManualIdentityPrepareSave();

    for (unsigned i = 0; i < SWOS_IDENTITY_SEED_COUNT; ++i)
        if (s_identity_records[i].id > highest) highest = s_identity_records[i].id;
    for (unsigned i = 0; i < s_generated_count; ++i) {
        const SWOS_GeneratedIdentity *g = &s_generated[i];
        if (g->id > highest) highest = g->id;
        if (!g->id || g->id <= SWOS_IDENTITY_SEED_COUNT ||
            g->slot > SWOS_GENERATED_ID_MAX_SLOT || g->age < 15u || g->age > 255u)
            ++errors;
        if (SWOS_RetiredIdentityContains(g->id)) { ++retired_generated; ++errors; }
        for (unsigned j = 0; j < i; ++j)
            if (s_generated[j].id == g->id ||
                (s_generated[j].team == g->team && s_generated[j].slot == g->slot)) {
                ++duplicate_generated; ++errors;
            }
    }
    for (unsigned i = 0; i < s_manual_count; ++i) {
        const SWOS_ManualIdentity *m = &s_manual_ids[i];
        if (!m->id) continue; /* tombstone: intentionally non-active */
        if (m->id > highest) highest = m->id;
        if (m->slot > 31u || m->age < 15u || m->age > 255u) ++errors;
        if (SWOS_RetiredIdentityContains(m->id)) { ++retired_manual; ++errors; }
        for (unsigned j = 0; j < i; ++j)
            if (s_manual_ids[j].id == m->id) { ++duplicate_manual; ++errors; }
        for (unsigned j = 0; j < s_generated_count; ++j)
            if ((s_generated[j].team == m->club && s_generated[j].slot == m->slot) ||
                s_generated[j].id == m->id) { ++conflicting_slots; ++errors; }
    }
    for (unsigned i = 0; i < s_retired_count; ++i)
        if (s_retired_ids[i].id > highest) highest = s_retired_ids[i].id;
    if (highest >= 4000000u || s_identity_next_id <= highest ||
        s_identity_next_id > 4000000u) ++errors;
    /* The identity structure, rather than failed-but-atomic swap attempts,
     * determines whether saving is safe. */
    unsigned canonical_valid = SWOS_IdentityCheck();
    if (!canonical_valid) ++errors;

    swos_age_log("CAREER_INTEGRITY_DETAIL",
        "phase=%s season=%u canonical_valid=%u swap_rejections=%u highest_id=%u next_id=%u",
        where, season, canonical_valid, s_identity_faults, highest,
        s_identity_next_id);

    swos_age_log("CAREER_INTEGRITY",
        "phase=%s season=%u repaired=%u errors=%u manual=%u->%u generated=%u->%u next=%u->%u retired_manual=%u retired_generated=%u duplicate_manual=%u duplicate_generated=%u conflicts=%u",
        where, season, repairs, errors, before_manual, s_manual_count,
        before_generated, s_generated_count, before_next, s_identity_next_id,
        retired_manual, retired_generated, duplicate_manual, duplicate_generated, conflicting_slots);
    /* Separate compact log for multi-season stress tests. */
    {
        FILE *f = fopen("swos_career_integrity.log", "a");
        if (f) {
            fprintf(f, "INTEGRITY phase=%s season=%u repairs=%u errors=%u original=%u generated=%u manual=%u retired=%u next_id=%u retired_active=%u duplicates=%u conflicts=%u result=%s\n",
                where, season, repairs, errors, (unsigned)SWOS_IDENTITY_SEED_COUNT,
                s_generated_count, s_manual_count, s_retired_count, s_identity_next_id,
                retired_manual + retired_generated,
                duplicate_manual + duplicate_generated, conflicting_slots,
                errors ? "BLOCK" : "OK");
            fclose(f);
        }
    }
    return errors == 0u;
}

unsigned SWOS_GeneratedIdentityRestore(const unsigned *rows,unsigned count,unsigned next_id){
    if(count>65536||next_id<SWOS_IDENTITY_SEED_COUNT+1||(!rows&&count))return 0;
    SWOS_GeneratedIdentity *candidate=count?malloc((size_t)count*sizeof(*candidate)):NULL;
    if(count&&!candidate)return 0;
    for(unsigned i=0;i<count;i++){
        unsigned team=rows[4*i],slot=rows[4*i+1],id=rows[4*i+2],age=rows[4*i+3];
        if(team>65535||slot>SWOS_GENERATED_ID_MAX_SLOT||id<=SWOS_IDENTITY_SEED_COUNT||id>=next_id||age>255){free(candidate);return 0;}
        for(unsigned j=0;j<i;j++)if(candidate[j].id==id||(candidate[j].team==team&&candidate[j].slot==slot)){free(candidate);return 0;}
        candidate[i]=(SWOS_GeneratedIdentity){team,slot,id,age};
    }
    free(s_generated);s_generated=candidate;s_generated_count=s_generated_capacity=count;
    s_identity_next_id=next_id;return 1;
}

/* Step 3E: side-effect-free cross-registry consistency audit.
 * The canonical AI registry intentionally retains original ID ownership
 * while a manual override follows a copied player. Such canonical/manual
 * mirrors are reported, not treated as an error until fully reconciled.
 * Do NOT use this audit to rewrite live identities or ages. */
unsigned SWOS_IdentityAudit(void)
{
    unsigned problems=0, mirrors=0, tombstones=0, manual_active=0;
    unsigned canonical_duplicate=0, cross_duplicate=0, age_mismatch=0;
    if(!s_identity_records){
        swos_age_log("IDENTITY_AUDIT","ready=0 reason=not_seeded");
        return 0;
    }
    for(unsigned i=0;i<s_generated_count;i++){
        const SWOS_GeneratedIdentity *g=&s_generated[i];
        if(g->id<=SWOS_IDENTITY_SEED_COUNT||g->id>=s_identity_next_id||g->age<15u||g->slot>31u)problems++;
        for(unsigned j=0;j<i;j++)if(g->id==s_generated[j].id||
            (g->team==s_generated[j].team&&g->slot==s_generated[j].slot))problems++;
    }
    for(unsigned i=0;i<s_manual_count;i++){
        const SWOS_ManualIdentity *m=&s_manual_ids[i];
        if(!m->id){tombstones++;continue;}
        manual_active++;
        if(m->id>=s_identity_next_id||m->age<15u||m->slot>31u)problems++;
        for(unsigned j=0;j<s_generated_count;j++)
            if(m->club==s_generated[j].team&&m->slot==s_generated[j].slot){cross_duplicate++;problems++;}
        for(unsigned j=0;j<i;j++)if(s_manual_ids[j].id&&s_manual_ids[j].id==m->id){problems++;cross_duplicate++;}
        for(unsigned j=0;j<SWOS_IDENTITY_SEED_COUNT;j++)if(s_identity_records[j].id==m->id){
            mirrors++;
            if(s_identity_records[j].age!=m->age)age_mismatch++;
            break;
        }
    }
    /* Step 4L: detailed live-location diagnostics. Never mutate any record.
     * Canonical/transfer mirrors are historical and not automatically duplicates.
     * An active generated ID in a different manual location IS suspicious. */
    unsigned duplicate_locations=0, overlapping_slots=0, retired_active=0;
    for(unsigned gi=0;gi<s_generated_count;gi++){
        const SWOS_GeneratedIdentity *g=&s_generated[gi];
        for(unsigned mi=0;mi<s_manual_count;mi++){
            const SWOS_ManualIdentity *m=&s_manual_ids[mi];
            if(g->team==m->club && g->slot==m->slot){
                overlapping_slots++;
                swos_age_log("DUP_SLOT_CONFLICT",
                    "generated_id=%u generated=%04X:%u manual_id=%u manual=%04X:%u",
                    g->id,g->team,g->slot,m->id,m->club,m->slot);
            }
            if(m->id && g->id==m->id){
                duplicate_locations++;
                swos_age_log("DUP_ACTIVE_ID",
                    "id=%u generated=%04X:%u manual=%04X:%u same_slot=%u",
                    g->id,g->team,g->slot,m->club,m->slot,
                    (unsigned)(g->team==m->club && g->slot==m->slot));
            }
        }
        if(SWOS_RetiredIdentityContains(g->id)){
            retired_active++;
            swos_age_log("DUP_RETIRED_ACTIVE",
                "id=%u registry=generated club=%04X slot=%u",
                g->id,g->team,g->slot);
        }
    }
    for(unsigned mi=0;mi<s_manual_count;mi++){
        const SWOS_ManualIdentity *m=&s_manual_ids[mi];
        if(m->id && SWOS_RetiredIdentityContains(m->id)){
            retired_active++;
            swos_age_log("DUP_RETIRED_ACTIVE",
                "id=%u registry=manual club=%04X slot=%u",
                m->id,m->club,m->slot);
        }
    }
    swos_age_log("DUP_AUDIT_SUMMARY",
        "generated=%u manual=%u duplicate_locations=%u overlapping_slots=%u retired_active=%u",
        s_generated_count,s_manual_count,duplicate_locations,overlapping_slots,retired_active);
    if(age_mismatch)problems+=age_mismatch;
    if(!SWOS_IdentityCheck()){canonical_duplicate=1;problems++;}
    swos_age_log("IDENTITY_AUDIT","ready=1 original=%u generated=%u manual_active=%u tombstones=%u canonical_mirrors=%u age_mismatches=%u cross_conflicts=%u canonical_invalid=%u problems=%u",
        (unsigned)SWOS_IDENTITY_SEED_COUNT,s_generated_count,manual_active,tombstones,mirrors,age_mismatch,cross_duplicate,canonical_duplicate,problems);
    return problems==0u;
}

static int SWOS_IdentityFind(uint32_t key)
{
    unsigned lo=0,hi=(unsigned)SWOS_IDENTITY_SEED_COUNT;
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;
        if(swos_identity_seed[mid].key<key)lo=mid+1;else hi=mid;
    }
    return lo<(unsigned)SWOS_IDENTITY_SEED_COUNT && swos_identity_seed[lo].key==key ? (int)lo : -1;
}
void SWOS_IdentityClear(void)
{
    free(s_identity_records);s_identity_records=NULL;
    free(s_retired_ids);s_retired_ids=NULL;s_retired_count=s_retired_capacity=0;
    free(s_generated);s_generated=NULL;s_generated_count=s_generated_capacity=0;
    free(s_manual_ids);s_manual_ids=NULL;s_manual_count=s_manual_capacity=0;
    s_identity_next_id=1;s_identity_faults=0;
    memset(s_identity_club_sources,0,sizeof(s_identity_club_sources));
}
static void SWOS_IdentityApplyCsvOverrides(void);
unsigned SWOS_IdentitySeed(void)
{
    if(s_identity_records)return 1;
    const unsigned n=(unsigned)SWOS_IDENTITY_SEED_COUNT;
    if(n>=UINT32_MAX-1)return 0;
    SWOS_TrackedIdentity *items=malloc((size_t)n*sizeof(*items));
    if(!items){swos_age_log("IDENTITY_SEED_FAIL","reason=allocation count=%u",n);return 0;}
    for(unsigned i=0;i<n;i++){
        items[i].id=i+1;items[i].age=swos_identity_seed[i].age;
        items[i].source=swos_identity_seed[i].source;
    }
    s_identity_records=items;s_identity_next_id=n+1;s_identity_faults=0;
    swos_age_log("IDENTITY_SEED","count=%u next_id=%u mode=ai_shadow",n,s_identity_next_id);
    return 1;
}
unsigned SWOS_IdentityCount(void){return s_identity_records?(unsigned)SWOS_IDENTITY_SEED_COUNT:0;}
unsigned SWOS_IdentityNextId(void){return s_identity_next_id;}
/* Step 4A: opt-in retirement eligibility simulation. No player is removed yet:
 * the AI shadow registry is a permutation of the original IDs, and real
 * replacement requires a new persistent identity and a roster override.
 * Keep this trial deterministic within a season when given an explicit seed. */
static unsigned SWOS_RetirementChance(unsigned age)
{
    if(age<35u)return 0u;
    if(age==35u)return 10u;
    if(age==36u)return 20u;
    if(age==37u)return 40u;
    if(age==38u)return 80u;
    return 90u;
}
static uint32_t SWOS_RetirementRandom(uint32_t *state)
{
    uint32_t x=*state;
    x^=x<<13;x^=x>>17;x^=x<<5;
    return *state=x;
}
static void SWOS_RetirementLiveAI(unsigned season);
static void SWOS_RetirementDryRun(unsigned season)
{
    const char *enabled=getenv("SWOS_RETIREMENT_DRY_RUN");
    if(!enabled||strcmp(enabled,"1")!=0)return;
    if(!s_identity_records){swos_age_log("RETIRE_DRY_RUN","ready=0");return;}
    uint32_t seed=0x534F5753u^(season*2654435761u);
    const char *setting=getenv("SWOS_RETIREMENT_SEED");
    if(setting&&*setting){char *end=NULL;unsigned long v=strtoul(setting,&end,0);
        if(end!=setting&&!*end)seed=(uint32_t)v;}
    if(!seed)seed=1u;
    unsigned inspected=0,eligible=0,selected=0,mirror_skip=0;
    unsigned per_age[6]={0}, chosen_age[6]={0};
    for(unsigned i=0;i<SWOS_IDENTITY_SEED_COUNT;i++){
        unsigned id=s_identity_records[i].id,age=s_identity_records[i].age;
        int mirrored=0;
        for(unsigned j=0;j<s_manual_count;j++)if(s_manual_ids[j].id==id){mirrored=1;break;}
        if(mirrored){mirror_skip++;continue;}
        inspected++;
        unsigned chance=SWOS_RetirementChance(age);
        if(!chance)continue;
        eligible++;
        unsigned band=age>=40u?5u:age-35u;
        per_age[band]++;
        unsigned roll=SWOS_RetirementRandom(&seed)%100u;
        if(roll<chance){selected++;chosen_age[band]++;
            if(selected<=12u)swos_age_log("RETIRE_DRY_CANDIDATE","kind=original id=%u age=%u chance=%u roll=%u",id,age,chance,roll);
        }
    }
    for(unsigned i=0;i<s_generated_count;i++){
        inspected++;unsigned age=s_generated[i].age,chance=SWOS_RetirementChance(age);
        if(!chance)continue;
        eligible++;unsigned band=age>=40u?5u:age-35u;per_age[band]++;
        unsigned roll=SWOS_RetirementRandom(&seed)%100u;
        if(roll<chance){selected++;chosen_age[band]++;
            if(selected<=12u)swos_age_log("RETIRE_DRY_CANDIDATE","kind=generated id=%u club=%04X slot=%u age=%u chance=%u roll=%u",s_generated[i].id,s_generated[i].team,s_generated[i].slot,age,chance,roll);
        }
    }
    for(unsigned i=0;i<s_manual_count;i++){
        if(!s_manual_ids[i].id)continue;
        inspected++;unsigned age=s_manual_ids[i].age,chance=SWOS_RetirementChance(age);
        if(!chance)continue;
        eligible++;unsigned band=age>=40u?5u:age-35u;per_age[band]++;
        unsigned roll=SWOS_RetirementRandom(&seed)%100u;
        if(roll<chance){selected++;chosen_age[band]++;
            if(selected<=12u)swos_age_log("RETIRE_DRY_CANDIDATE","kind=manual id=%u club=%04X slot=%u age=%u chance=%u roll=%u",s_manual_ids[i].id,s_manual_ids[i].club,s_manual_ids[i].slot,age,chance,roll);
        }
    }
    swos_age_log("RETIRE_DRY_RUN","season=%u inspected=%u eligible=%u would_retire=%u canonical_mirrors_skipped=%u age35=%u/%u age36=%u/%u age37=%u/%u age38=%u/%u age39=%u/%u age40plus=%u/%u replacements=0 mode=simulation_only",
        season,inspected,eligible,selected,mirror_skip,chosen_age[0],per_age[0],chosen_age[1],per_age[1],chosen_age[2],per_age[2],chosen_age[3],per_age[3],chosen_age[4],per_age[4],chosen_age[5],per_age[5]);
}

void SWOS_IdentityAdvanceSeason(void)
{
    if(!s_identity_records && !SWOS_IdentitySeed())return;
    unsigned originalChanged=0,generatedChanged=0,manualChanged=0;
    unsigned originalAtLimit=0,generatedAtLimit=0,manualAtLimit=0;
    unsigned progressionProblems=0;
    for(unsigned i=0;i<SWOS_IDENTITY_SEED_COUNT;i++){
        unsigned before=s_identity_records[i].age;
        if(before<255u){s_identity_records[i].age++;originalChanged++;}
        else originalAtLimit++;
        if(s_identity_records[i].age!=(before<255u?before+1u:before))progressionProblems++;
    }
    for(unsigned i=0;i<s_generated_count;i++){
        unsigned before=s_generated[i].age;
        if(before<255u){s_generated[i].age++;generatedChanged++;}
        else generatedAtLimit++;
        if(s_generated[i].age!=(before<255u?before+1u:before))progressionProblems++;
    }
    for(unsigned i=0;i<s_manual_count;i++){
        if(!s_manual_ids[i].id)continue;
        unsigned before=s_manual_ids[i].age;
        if(before<255u){s_manual_ids[i].age++;manualChanged++;}
        else manualAtLimit++;
        if(s_manual_ids[i].age!=(before<255u?before+1u:before))progressionProblems++;
    }
    swos_age_log("GENERATED_ID_SEASON_AGE","count=%u",s_generated_count);
    swos_age_log("IDENTITY_SEASON_AGE","count=%u mode=ai_shadow",(unsigned)SWOS_IDENTITY_SEED_COUNT);
    swos_age_log("IDENTITY_AGE_PROGRESSION",
        "original=%u generated=%u manual=%u original_at_limit=%u generated_at_limit=%u manual_at_limit=%u problems=%u",
        originalChanged,generatedChanged,manualChanged,originalAtLimit,generatedAtLimit,manualAtLimit,progressionProblems);
}

unsigned SWOS_IdentityGet(unsigned i,unsigned out[4])
{
    if(!out||!s_identity_records||i>=SWOS_IDENTITY_SEED_COUNT)return 0;
    out[0]=swos_identity_seed[i].key;out[1]=s_identity_records[i].id;
    out[2]=s_identity_records[i].age;out[3]=s_identity_records[i].source;return 1;
}
unsigned SWOS_IdentitySwap(unsigned fa,unsigned ta,unsigned sa,unsigned fb,unsigned tb,unsigned sb)
{
    if(!s_identity_records&&!SWOS_IdentitySeed())return 0;
    if(fa>255||ta>255||sa>15||fb>255||tb>255||sb>15)return 0;
    int a=SWOS_IdentityFind(((uint32_t)fa<<12)|((uint32_t)ta<<4)|sa);
    int b=SWOS_IdentityFind(((uint32_t)fb<<12)|((uint32_t)tb<<4)|sb);
    if(a<0||b<0||a==b){s_identity_faults++;
        swos_age_log("IDENTITY_SWAP_FAIL","file_a=%u team_a=%u slot_a=%u file_b=%u team_b=%u slot_b=%u",fa,ta,sa,fb,tb,sb);
        return 0;
    }
    SWOS_TrackedIdentity tmp=s_identity_records[a];
    s_identity_records[a]=s_identity_records[b];s_identity_records[b]=tmp;
    return 1;
}
unsigned SWOS_IdentityCheck(void)
{
    /* A failed swap leaves the canonical records unchanged. It is a rejected
     * operation, not evidence of a corrupt registry. In particular, AI
     * optimiser attempts against unmapped slots must not permanently poison
     * every later career save. Validate actual records below instead. */
    if(!s_identity_records)return 0;
    const unsigned n=(unsigned)SWOS_IDENTITY_SEED_COUNT;
    /* Retirement replacements receive fresh IDs above the original seed range.
     * The canonical locations remain fixed, but the occupants need not be a
     * permutation of the initial 23,264 players.  Bound allocation even when
     * a corrupt CAR supplies an unreasonable next_id. */
    if(s_identity_next_id<=n||s_identity_next_id>4000000u)return 0;
    uint8_t *seen=calloc((size_t)s_identity_next_id,1);
    if(!seen)return 0;
    unsigned valid=1;
    for(unsigned i=0;i<n;i++){
        unsigned id=s_identity_records[i].id;
        if(!id||id>=s_identity_next_id||seen[id]||s_identity_records[i].source>2){valid=0;break;}
        seen[id]=1;
    }
    free(seen);return valid;
}
/* Transactional restoration: rejects missing, duplicate, or reordered keys,
 * preserving the existing registry when the supplied save is invalid. */
unsigned SWOS_IdentityRestore(const unsigned *records,unsigned count,unsigned next_id)
{
    const unsigned n=(unsigned)SWOS_IDENTITY_SEED_COUNT;
    if(!records||count!=n||next_id<=n||next_id>4000000u)return 0;
    SWOS_TrackedIdentity *candidate=malloc((size_t)n*sizeof(*candidate));
    uint8_t *seen=calloc((size_t)next_id,1);
    if(!candidate||!seen){free(candidate);free(seen);return 0;}
    unsigned valid=1;
    for(unsigned i=0;i<n;i++){
        unsigned key=records[4*i],id=records[4*i+1],age=records[4*i+2],source=records[4*i+3];
        if(key!=swos_identity_seed[i].key||!id||id>=next_id||seen[id]||age>255||source>2){valid=0;break;}
        seen[id]=1;
        candidate[i]=(SWOS_TrackedIdentity){id,(uint8_t)age,(uint8_t)source};
    }
    free(seen);
    if(!valid){free(candidate);return 0;}
    free(s_identity_records);s_identity_records=candidate;
    s_identity_next_id=next_id;s_identity_faults=0;return 1;
}

/* Step 4B.1: allocate a permanent identity for a future roster replacement.
 * The caller MUST first ensure a corresponding replacement PlayerFile can be
 * persisted, then call this on successful roster commit.  Not wired to the
 * dry run, because mutating IDs alone would orphan real player records.
 * The old ID remains unallocated forever because next_id is monotonic.
 */
unsigned SWOS_IdentityReplaceCanonical(unsigned file,unsigned record,
                                        unsigned slot,unsigned age,
                                        unsigned *retired_id)
{
    if(retired_id)*retired_id=0;
    if(file>255u||record>255u||slot>=16u||age>255u||
       !s_identity_records||s_identity_next_id<=SWOS_IDENTITY_SEED_COUNT||
       s_identity_next_id>=4000000u)return 0;
    int index=SWOS_IdentityFind(((uint32_t)file<<12)|((uint32_t)record<<4)|slot);
    if(index<0)return 0;
    unsigned old=s_identity_records[index].id;
    unsigned fresh=s_identity_next_id++;
    s_identity_records[index].id=fresh;
    s_identity_records[index].age=(uint8_t)age;
    s_identity_records[index].source=0;
    if(retired_id)*retired_id=old;
    swos_age_log("IDENTITY_CANONICAL_REPLACE","file=%u record=%u slot=%u retired_id=%u new_id=%u age=%u",
                 file,record,slot,old,fresh,age);
    return fresh;
}

/* Until AI-generated youths have an atomic cross-registry exchange,
 * never swap their physical record with a canonical/manual footballer.
 * The old sequence moved generated IDs before resolving manual overlays,
 * creating ambiguous double claims. This guard prevents corrupt saves. */
static int SWOS_AI_P3HasGeneratedSlot(unsigned club,unsigned slot)
{
    unsigned row[4];
    for (unsigned g=0;g<SWOS_GeneratedIdentityCount();g++)
        if (SWOS_GeneratedIdentityGet(g,row) && row[0]==club && row[1]==slot)
            return 1;
    return 0;
}

static int SWOS_AI_P3Swap(SWOS_AI_P3Club *a, unsigned sa,
                          SWOS_AI_P3Club *b, unsigned sb)
{
    unsigned ia = SWOS_AI_P3OverrideIndex(a->id, sa);
    unsigned ib = SWOS_AI_P3OverrideIndex(b->id, sb);
    unsigned needed = (ia == _numForeignMarketPlayers2) +
                      (ib == _numForeignMarketPlayers2);
    if (_numForeignMarketPlayers2 + needed > SWOS_AI_CAREER_POOL_CAPACITY)
        return 0;
    /* A physical player swap must never precede a failed canonical identity
     * swap. Some expanded TEAM data files (notably TEAM.047) are absent from
     * the compiled canonical seed. Earlier code exchanged the PlayerFiles,
     * recorded an exchange journal entry, then discovered that the canonical
     * identities could not be exchanged. This left ages and retirements tied
     * to the wrong player and could eventually poison the save. Until those
     * files receive a compatible identity migration, decline the exchange
     * atomically rather than corrupting either squad. */
    if (SWOS_AI_P3HasGeneratedSlot(a->id,sa) ||
        SWOS_AI_P3HasGeneratedSlot(b->id,sb)) {
        swos_age_log("AI_SWAP_GENERATED_DEFER",
            "club_a=%04X slot_a=%u club_b=%04X slot_b=%u",
            a->id,sa,b->id,sb);
        return 0;
    }
    unsigned ageA=0,idA=0,ageB=0,idB=0;
    if (!SWOS_IdentitySeed() ||
        !SWOS_IdentityAgeForClub(a->id,sa,&ageA,&idA) ||
        !SWOS_IdentityAgeForClub(b->id,sb,&ageB,&idB) || !idA || !idB || idA==idB) {
        swos_age_log("AI_SWAP_UNMAPPED_REJECT",
            "club_a=%04X file_a=%u rec_a=%u slot_a=%u club_b=%04X file_b=%u rec_b=%u slot_b=%u",
            a->id, a->sourceFile, a->sourceRecord, sa,
            b->id, b->sourceFile, b->sourceRecord, sb);
        return 0;
    }
    if (!SWOS_ManualIdentityAISwap(a->id,sa,b->id,sb)) {
        swos_age_log("AI_SWAP_IDENTITY_ABORT",
            "club_a=%04X slot_a=%u club_b=%04X slot_b=%u",
            a->id,sa,b->id,sb);
        return 0;
    }
    PlayerFile pa = a->players[sa];
    PlayerFile pb = b->players[sb];
    /* After appending first entry the next new slot is one index later. */
    if (ia == _numForeignMarketPlayers2 && ib == _numForeignMarketPlayers2)
        ++ib;
    SWOS_AI_P3WriteOverrideAt(a->id, sa, pb, ia);
    SWOS_AI_P3WriteOverrideAt(b->id, sb, pa, ib);
    a->players[sa] = pb;
    b->players[sb] = pa;
    /* Keep the per-player cooldown key with the actual player, not the
     * physical slot or its mutable attributes. */
    uint64_t cooldown = a->cooldownKeys[sa];
    a->cooldownKeys[sa] = b->cooldownKeys[sb];
    b->cooldownKeys[sb] = cooldown;
    SWOS_GeneratedIdentityAISwap(a->id,sa,b->id,sb);

    /* Canonical-to-canonical swaps update the fixed seed. Any swap involving
     * TEAM.047 uses the existing persistent manual overlay instead. */
    int bothCanonical=SWOS_IdentityFind(((uint32_t)a->sourceFile<<12)|
                     ((uint32_t)a->sourceRecord<<4)|sa)>=0 &&
                      SWOS_IdentityFind(((uint32_t)b->sourceFile<<12)|
                     ((uint32_t)b->sourceRecord<<4)|sb)>=0;
    if (bothCanonical && !SWOS_IdentitySwap(a->sourceFile,a->sourceRecord,sa,
                                             b->sourceFile,b->sourceRecord,sb))
        swos_age_log("IDENTITY_AI_SWAP_ERROR","id_a=%04X id_b=%04X",a->id,b->id);
    /* Record only an exchange that was successfully applied. The source
     * coordinates distinguish clubs with colliding global team numbers. */
    if (!SWOS_AI_IdentityJournalAdd(a->sourceFile, a->sourceRecord, sa,
                                    b->sourceFile, b->sourceRecord, sb))
        swos_age_log("AI_ID_JOURNAL_FAIL", "file_a=%u rec_a=%u file_b=%u rec_b=%u",
                     a->sourceFile, a->sourceRecord, b->sourceFile, b->sourceRecord);

    unsigned syncedA = Career_SyncClubPlayerCopies(a->id, sa, &pb);
    unsigned syncedB = Career_SyncClubPlayerCopies(b->id, sb, &pa);
    SWOS_AI_LOG("[SWOS AI CACHE] sync %04X:%u cached=%u; %04X:%u cached=%u\n",
                a->id, sa, syncedA, b->id, sb, syncedB);
    /* Diagnostic only: source player records have been exchanged.  This
     * does NOT assert permanent identity tracking is active. */
    swos_age_log("AI_PLAYER_SWAP", "team_a=%04X slot_a=%u team_b=%04X slot_b=%u live_a=%u live_b=%u",
                 (unsigned)a->id, sa, (unsigned)b->id, sb, syncedA, syncedB);
    a->usedSlots[sa] = b->usedSlots[sb] = 1;
    return 1;
}

/* After AI transfers, make sure a more valuable substitute is not left on
 * the bench behind a weaker player in exactly the same position. The first
 * eleven entries of playerNumbers are the starters, NOT physical slots 0..10.
 * Never run
 * this for the human club. Use actual game market values, not price indices.
 *
 * Unlike a visual reordering, this is a physical two-slot swap: foreign
 * overrides, all live cache copies, canonical/manual/generated identities and
 * the exchange journal must move together so age and transfer history follow
 * the footballer. No new save fields are required. */
static int SWOS_AI_P3PromoteSubstitute(SWOS_AI_P3Club *club,
                                        unsigned starter, unsigned substitute)
{
    if (!club || !club->lineupValid || club->id == _selTeamNumber ||
        starter >= 16u || substitute >= 16u || starter == substitute)
        return 0;
    PlayerFile first = club->players[starter];
    PlayerFile bench = club->players[substitute];
    if (!SWOS_AI_P3IsValid(first) || !SWOS_AI_P3IsValid(bench) ||
        SWOS_AI_P3Position(first) != SWOS_AI_P3Position(bench) ||
        SWOS_AI_P3Value(bench) <= SWOS_AI_P3Value(first)) return 0;
    /* RES/TRIAL is a status rather than a bench ordering. Do not force such
     * players into the starting team before the game promotes them. */
    if ((uint8_t)bench.form == PLAYER_FORM_RESERVE ||
        (uint8_t)bench.form == PLAYER_FORM_TRIALIST) return 0;
    unsigned firstIndex = SWOS_AI_P3OverrideIndex(club->id, starter);
    unsigned benchIndex = SWOS_AI_P3OverrideIndex(club->id, substitute);
    unsigned count = _numForeignMarketPlayers2;
    unsigned needed = (firstIndex == count) + (benchIndex == count);
    if (count + needed > SWOS_AI_CAREER_POOL_CAPACITY) return 0;
    /* As with international AI exchanges, do not physically reorder a
     * line-up unless BOTH source slots exist in the canonical seed. A
     * missing slot would leave its player age at the previous position. */
    if (SWOS_AI_P3HasGeneratedSlot(club->id,starter) ||
        SWOS_AI_P3HasGeneratedSlot(club->id,substitute)) return 0;
    unsigned ageStart=0,idStart=0,ageSub=0,idSub=0;
    if (!SWOS_IdentitySeed() ||
        !SWOS_IdentityAgeForClub(club->id,starter,&ageStart,&idStart) ||
        !SWOS_IdentityAgeForClub(club->id,substitute,&ageSub,&idSub) ||
        !idStart || !idSub || idStart==idSub) {
        swos_age_log("LINEUP_SWAP_UNMAPPED_REJECT",
            "club=%04X file=%u rec=%u start=%u bench=%u",
            club->id, club->sourceFile, club->sourceRecord, starter, substitute);
        return 0;
    }
    if (!SWOS_ManualIdentityAISwap(club->id,starter,club->id,substitute)) {
        swos_age_log("LINEUP_SWAP_IDENTITY_ABORT",
            "club=%04X starter=%u substitute=%u",
            club->id,starter,substitute);
        return 0;
    }
    if (firstIndex == count && benchIndex == count) ++benchIndex;
    SWOS_AI_P3WriteOverrideAt(club->id, starter, bench, firstIndex);
    SWOS_AI_P3WriteOverrideAt(club->id, substitute, first, benchIndex);
    club->players[starter] = bench;
    club->players[substitute] = first;
    SWOS_GeneratedIdentityAISwap(club->id, starter, club->id, substitute);

    int bothCanonical=SWOS_IdentityFind(((uint32_t)club->sourceFile<<12)|
                      ((uint32_t)club->sourceRecord<<4)|starter)>=0 &&
                      SWOS_IdentityFind(((uint32_t)club->sourceFile<<12)|
                      ((uint32_t)club->sourceRecord<<4)|substitute)>=0;
    if (bothCanonical && !SWOS_IdentitySwap(club->sourceFile, club->sourceRecord, starter,
                                             club->sourceFile, club->sourceRecord, substitute))
        swos_age_log("IDENTITY_LINEUP_SWAP_FAIL", "club=%04X start=%u bench=%u",
                     club->id, starter, substitute);
    if (!SWOS_AI_IdentityJournalAdd(club->sourceFile, club->sourceRecord, starter,
                                    club->sourceFile, club->sourceRecord, substitute))
        swos_age_log("LINEUP_ID_JOURNAL_FAIL", "club=%04X start=%u bench=%u",
                     club->id, starter, substitute);
    Career_SyncClubPlayerCopies(club->id, starter, &bench);
    Career_SyncClubPlayerCopies(club->id, substitute, &first);
    club->usedSlots[starter] = club->usedSlots[substitute] = 1;
    SWOS_AI_LOG("[SWOS AI LINEUP] PROMOTE club=%s [%04X] start=%u bench=%u position=%u starter_value=%u substitute_value=%u\n",
                club->name, club->id, starter, substitute,
                SWOS_AI_P3Position(bench), SWOS_AI_P3Value(first),
                SWOS_AI_P3Value(bench));
    return 1;
}

/* Greedy strictly improving swaps. Each promotion increases the value of
 * the starting eleven; there are at most five promoted bench players. */
static unsigned SWOS_AI_P3OptimiseLineup(SWOS_AI_P3Club *club)
{
    if (!club || !club->lineupValid) return 0;
    unsigned changes = 0;
    for (;;) {
        unsigned bestStarter = 16u, bestBench = 16u, bestGain = 0;
        for (unsigned startingIndex = 0; startingIndex < 11u; ++startingIndex) {
            unsigned s = club->lineupSlots[startingIndex];
            const PlayerFile *first = &club->players[s];
            if (!SWOS_AI_P3IsValid(*first)) continue;
            unsigned firstValue = SWOS_AI_P3Value(*first);
            for (unsigned benchIndex = 11u; benchIndex < 16u; ++benchIndex) {
                unsigned b = club->lineupSlots[benchIndex];
                const PlayerFile *bench = &club->players[b];
                if (!SWOS_AI_P3IsValid(*bench) ||
                    (uint8_t)bench->form == PLAYER_FORM_RESERVE ||
                    (uint8_t)bench->form == PLAYER_FORM_TRIALIST ||
                    SWOS_AI_P3Position(*first) != SWOS_AI_P3Position(*bench))
                    continue;
                unsigned benchValue = SWOS_AI_P3Value(*bench);
                if (benchValue > firstValue && benchValue - firstValue > bestGain) {
                    bestGain = benchValue - firstValue;
                    bestStarter = s;
                    bestBench = b;
                }
            }
        }
        if (!bestGain || !SWOS_AI_P3PromoteSubstitute(club, bestStarter, bestBench))
            break;
        ++changes;
        /* A substitute can only enter the first eleven once. Guard against
         * corrupted values or unexpectedly cyclic roster metadata. */
        if (changes >= 5u) break;
    }
    return changes;
}

static unsigned SWOS_AI_P3AuditSlot(uint16_t id, unsigned slot,
                                    const PlayerFile *expected,
                                    unsigned *cacheChecked)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    extern uint16_t _numSelectedTeams, _careerFileBuffer, _currentTeamNumber;
    extern uint8_t _tmdFileBuffer[], _currentTeam[];
    unsigned fails = 0;
    unsigned idx = SWOS_AI_P3OverrideIndex(id, slot);
    if (idx >= _numForeignMarketPlayers2 ||
        memcmp(&_careerForeignMarketPlayers[idx].player, expected, sizeof(*expected))) {
        SWOS_AI_LOG("[SWOS AI AUDIT] FAIL override club=%04X slot=%u\n", id, slot);
        ++fails;
    }
    unsigned n = _numSelectedTeams <= SELECTED_TEAMS_MAX ? _numSelectedTeams : 0;
    for (unsigned i=0;i<n;i++) {
        TeamFileEx *t = &_selectedTeams[i];
        if (PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal) != id) continue;
        ++*cacheChecked;
        if (memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
    }
    n = _careerFileBuffer <= TMD_POOL_RECORDS ? _careerFileBuffer : 0;
    TeamFileEx *tmd=(TeamFileEx *)_tmdFileBuffer;
    for(unsigned i=0;i<n;i++) {
        TeamFileEx *t=&tmd[i];
        if(PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal)!=id) continue;
        ++*cacheChecked;
        if(memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
    }
    uint16_t parkedCount;
    const TeamFileEx *parked = Career_ParkedTeams(&parkedCount);
    for (unsigned i = 0; i < parkedCount; ++i) {
        if (PACK_BE_U16(parked[i].team.countryNumber, parked[i].team.teamOrdinal) != id)
            continue;
        ++*cacheChecked;
        if (memcmp(&parked[i].players[slot], expected, sizeof *expected))
            ++fails;
    }
    if(_currentTeamNumber==id) {
        TeamFileEx *t=(TeamFileEx *)_currentTeam;
        if(PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal)==id) {
            ++*cacheChecked;
            if(memcmp(&t->players[slot],expected,sizeof(*expected))) ++fails;
        }
    }
    if(fails) SWOS_AI_LOG("[SWOS AI AUDIT] FAIL club=%04X slot=%u problems=%u\n",id,slot,fails);
    return fails;
}

static int SWOS_AI_P3MissingCountry(unsigned c)
{
    /* TEAM.072 contains custom/joke preset teams, not career clubs.
     * TEAM.080--085 are national-team continents and are excluded by
     * the collector's country <= 79 loop bound. Keep both outside AI swaps. */
    return (c >= 52 && c <= 54) || c == 56 ||
           c == 58 || c == 61 || c == 63 || c == 74 ||
           c == 57 || c == 59 || c == 70 || c == 72;
}

/* Load snapshots without LoadTeamFile(): the game's asset loader can retry
 * forever when a country is absent/unavailable, and changes global state.
 * Respect SWOS_DATA_JSON by trying the same JSON data loader first. The
 * fallback directly reads verified, raw TEAM.NNN bytes from disk. */
static int SWOS_AI_P3ReadCountry(unsigned country, uint8_t *buf, size_t cap,
                                  uint16_t *outCount)
{
    char filename[80];
    FILE *f = NULL;
    size_t size = 0;
    if (!buf || cap < 2 || !outCount) return 0;
    *outCount = 0;
    const char *json = getenv("SWOS_DATA_JSON");
    if (!(json && strcmp(json, "0") == 0)) {
        extern int DataJson_LoadTeamFileForCountry(int, uint8_t *, int);
        int n = DataJson_LoadTeamFileForCountry((int)country, buf, 93);
        if (n > 0 && n <= 93) {
            *outCount = (uint16_t)n;
            return 1;
        }
    }
    /* Resolve the same asset directory and case as OpenSWOS's loader.
     * The working directory is not necessarily the database directory. */
    char logical[40];
    char resolved[1024];
    snprintf(logical, sizeof logical, "data/team.%03u", country);
    const char *actual = swos_data_path(logical, resolved, sizeof resolved);
    f = fopen(actual, "rb");
    if (!f) {
        SWOS_AI_LOG("[SWOS AI WORLD D4] SKIP country=%u: cannot open %s\n",
                    country, actual);
        return 0;
    }
    SWOS_AI_LOG("[SWOS AI WORLD D4] OPEN country=%u path=%s\n", country, actual);
    /* TEAM.NNN files may be raw (2+684*N) or RNC-compressed. Read into a
     * separate bounded input buffer and decode to the validated snapshot.
     * In particular, never put compressed bytes in the output team buffer. */
    const size_t packedCap = 256u * 1024u;
    uint8_t *packed = (uint8_t *)malloc(packedCap + 32u);
    if (!packed) { fclose(f); return 0; }
    size = fread(packed, 1, packedCap + 1u, f);
    int io_error = ferror(f);
    fclose(f);
    if (io_error || size == 0 || size > packedCap) {
        SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: oversized/unreadable input size=%u\n",
                    country, (unsigned)size);
        free(packed);
        return 0;
    }
    if (size >= 18 && packed[0] == 'R' && packed[1] == 'N' && packed[2] == 'C') {
        unsigned method = packed[3];
        uint32_t unpacked = BE_READ_U32(packed + 4);
        uint32_t compressed = BE_READ_U32(packed + 8);
        if ((method != 1 && method != 2) || unpacked < 2 ||
            unpacked > cap || compressed > size - 18u || compressed == 0 ||
            (unpacked - 2u) % sizeof(TeamFileEx) != 0 ||
            (unpacked - 2u) / sizeof(TeamFileEx) > 93u) {
            SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: invalid RNC header method=%u unpacked=%u packed=%u size=%u\n",
                        country, method, unpacked, compressed, (unsigned)size);
            free(packed);
            return 0;
        }
        /* Decoder may read a few bytes past the last compressed word. */
        memset(packed + size, 0, 32);
        if (method == 1) RncDecompressV1(packed, buf);
        else             RncDecompressV2(packed, buf);
        size = unpacked;
        SWOS_AI_LOG("[SWOS AI WORLD D5] DECOMPRESSED country=%u method=%u bytes=%u\n",
                    country, method, (unsigned)size);
    } else {
        if (size > cap) {
            SWOS_AI_LOG("[SWOS AI WORLD D5] SKIP country=%u: raw data too large size=%u\n",
                        country, (unsigned)size);
            free(packed);
            return 0;
        }
        memcpy(buf, packed, size);
        SWOS_AI_LOG("[SWOS AI WORLD D5] RAW country=%u bytes=%u\n",
                    country, (unsigned)size);
    }
    free(packed);
    if (size < 2) return 0;
    unsigned count = ((unsigned)buf[0] << 8) | buf[1];
    if (!count || count > 93 || size != 2u + count * sizeof(TeamFileEx)) {
        SWOS_AI_LOG("[SWOS AI WORLD D3] SKIP country=%u: count=%u size=%u expected=%u\n",
                    country, count, (unsigned)size,
                    (unsigned)(2u + count * sizeof(TeamFileEx)));
        return 0;
    }
    /* Raw TEAM.NNN records use BE for the count and global team number.
     * JSON gives host-native records; only binary records reach this point. */
    TeamFileEx *rec = (TeamFileEx *)(buf + 2);
    for (unsigned i = 0; i < count; ++i)
        rec[i].team.globalTeamNumber =
            (int16_t)BE_READ_U16(&rec[i].team.globalTeamNumber);
    *(uint16_t *)buf = (uint16_t)count;
    *outCount = (uint16_t)count;
    return 1;
}

/* Competition browsers only need names. Read the team file without
 * projecting the national squads or changing the live selection buffer. */
int Career_CopyTeamName(uint16_t teamId, char name[18])
{
    static uint8_t snapshot[2 + 93 * sizeof(TeamFileEx)];
    uint16_t count;
    if (SWOS_AI_P3ReadCountry(teamId >> 8, snapshot, sizeof snapshot, &count)) {
        const TeamFileEx *team = (const TeamFileEx *)(snapshot + 2);
        for (unsigned i = 0; i < count; ++i) {
            if (BE_READ_U16(&team[i]) == teamId) {
                SWOS_CopyTeamName18(name, team[i].team.teamName);
                if (World_Current() == WORLD_2627 && teamId == 0x5030 &&
                    !strcmp(name, "YUGOSLAVIA"))
                    SWOS_CopyTeamName18(name, "SERBIA");
                return 1;
            }
        }
    }
    if (World_Current() == WORLD_9697 && teamId == PACK_BE_U16(CN_EUROPE, 0x0C)) {
        /* The classic national picker adds Bosnia to unpatched team files. */
        SWOS_CopyTeamName18(name, "BOSNIA");
        return 1;
    }
    return 0;
}

/* At the start of a NEW career, apply the user-editable ages CSV to the
 * canonical identity registry. Previously swos_identity_seed.h held older
 * compiled ages and took precedence over the CSV in the squad renderer,
 * transfer lists and retirement system. Existing saved careers deliberately
 * keep their persisted identity ages and never call this function. */
static void SWOS_IdentityApplyCsvOverrides(void)
{
    extern int SWOS_AgesLookupExact(const char *name, unsigned *age);
    if (!s_identity_records) return;
    size_t cap = 2u + 93u * sizeof(TeamFileEx);
    uint8_t *snapshot = (uint8_t *)malloc(cap);
    if (!snapshot) return;
    unsigned updated = 0, matched = 0, examined = 0;
    for (unsigned country = 0; country < 80u; ++country) {
        uint16_t count = 0;
        if (!SWOS_AI_P3ReadCountry(country, snapshot, cap, &count)) continue;
        const TeamFileEx *teams = (const TeamFileEx *)(snapshot + 2);
        for (unsigned record = 0; record < count; ++record) {
            for (unsigned slot = 0; slot < 16u; ++slot) {
                uint32_t key = (country << 12) | (record << 4) | slot;
                int index = SWOS_IdentityFind(key);
                if (index < 0) continue;
                ++examined;
                const char *name = (const char *)teams[record].players[slot].playerName;
                unsigned csvAge = 0;
                if (!SWOS_AgesLookupExact(name, &csvAge)) continue;
                ++matched;
                if (s_identity_records[index].age != csvAge) {
                    unsigned oldAge = s_identity_records[index].age;
                    s_identity_records[index].age = (uint8_t)csvAge;
                    ++updated;
                    swos_age_log("IDENTITY_CSV_AGE_OVERRIDE",
                        "file=%u record=%u slot=%u name=%.22s old=%u new=%u",
                        country, record, slot, name, oldAge, csvAge);
                }
            }
        }
    }
    free(snapshot);
    swos_age_log("IDENTITY_CSV_AGE_SYNC",
        "examined=%u matched=%u updated=%u", examined, matched, updated);
}

/* Resolve one club on demand before its first manual transfer.
 * Avoid guessing the canonical TEAM record from its global ordinal. */
static void SWOS_IdentityResolveClub(unsigned club)
{
    if(club>65535u || s_identity_club_sources[club])return;
    unsigned country=club>>8;
    if(country>79u || SWOS_AI_P3MissingCountry(country))return;
    size_t cap=2u+93u*sizeof(TeamFileEx);
    uint8_t *snapshot=malloc(cap);
    if(!snapshot)return;
    uint16_t count=0;
    if(SWOS_AI_P3ReadCountry(country,snapshot,cap,&count)){
        const TeamFileEx *teams=(const TeamFileEx*)(snapshot+2);
        for(unsigned i=0;i<count;i++){
            if(teams[i].team.countryNumber!=country)continue;
            unsigned id=(country<<8)|(uint8_t)teams[i].team.teamOrdinal;
            if(id==club){SWOS_IdentityRegisterClub(id,country,i);break;}
        }
    }
    free(snapshot);
}

/* Ensure the two manually expanded TEAM files are registered even before
 * AI transfers or the world-player search touch them. TEAM.009 is already
 * fully canonical; TEAM.047 uses persistent manual IDs for its 224 original
 * slots. Source mapping always uses verified TEAM record numbers, never team
 * ordinals. This also covers loading older careers which lack the extensions. */
static void SWOS_IdentityBootstrapExpandedTeams(void)
{
    if (!s_identity_records) return;
    const unsigned countries[2]={9u,47u};
    const size_t cap=2u+93u*sizeof(TeamFileEx);
    uint8_t *snapshot=malloc(cap);
    if (!snapshot) return;
    for (unsigned f=0;f<2u;f++) {
        unsigned country=countries[f],mapped=0,canonical=0,missing=0;
        uint16_t count=0;
        if (!SWOS_AI_P3ReadCountry(country,snapshot,cap,&count)) {
            swos_age_log("IDENTITY_EXPANSION_ASSET_MISSING","file=%u",country);
            continue;
        }
        const TeamFileEx *teams=(const TeamFileEx *)(snapshot+2);
        for (unsigned record=0;record<count;record++) {
            if (teams[record].team.countryNumber!=country) continue;
            unsigned club=(country<<8)|(uint8_t)teams[record].team.teamOrdinal;
            SWOS_IdentityRegisterClub(club,country,record);
            mapped++;
            for (unsigned slot=0;slot<16u;slot++) {
                uint32_t key=((uint32_t)country<<12)|
                             ((uint32_t)record<<4)|slot;
                if (SWOS_IdentityFind(key)>=0) canonical++;
                else missing++;
            }
        }
        swos_age_log("IDENTITY_EXPANSION_AUDIT",
            "file=%u clubs=%u canonical_slots=%u extension_slots=%u",
            country,mapped,canonical,missing);
    }
    free(snapshot);
}

/* A saved career may have changed a club's displayed starting XI.
 * The asset TEAM.NNN ordering is not authoritative once selectedTeams or
 * the TMD career cache contains an updated playerNumbers permutation. */
static int SWOS_AI_P3ValidLineup(const uint8_t *slots)
{
    uint16_t seen = 0;
    for (unsigned i = 0; i < 16u; ++i) {
        unsigned slot = slots[i];
        if (slot >= 16u || (seen & (uint16_t)(1u << slot))) return 0;
        seen |= (uint16_t)(1u << slot);
    }
    return 1;
}

static void SWOS_AI_P3CopyCareerLineup(SWOS_AI_P3Club *club)
{
    extern uint16_t _numSelectedTeams, _careerFileBuffer, _currentTeamNumber;
    extern uint8_t _tmdFileBuffer[], _currentTeam[];
    const TeamFileEx *chosen = NULL;
    const char *source = "TEAM";
    /* Current cached team is the authoritative record when present. */
    if (_currentTeamNumber == club->id) {
        const TeamFileEx *t = (const TeamFileEx *)_currentTeam;
        if (PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal) == club->id &&
            SWOS_AI_P3ValidLineup(t->team.playerNumbers)) {
            chosen = t;
            source = "CURRENT";
        }
    }
    /* Selected-team records have priority over season TMD copies, matching
     * the usual GetTeamPointer lookup precedence. */
    if (!chosen) {
        unsigned n = _numSelectedTeams < 100u ? _numSelectedTeams : 100u;
        for (unsigned i = 0; i < n; ++i) {
            const TeamFileEx *t = &_selectedTeams[i];
            if (PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal) == club->id &&
                SWOS_AI_P3ValidLineup(t->team.playerNumbers)) {
                chosen = t;
                source = "SELECTED";
                break;
            }
        }
    }
    if (!chosen) {
        const TeamFileEx *tmd = (const TeamFileEx *)_tmdFileBuffer;
        unsigned n = _careerFileBuffer < 128u ? _careerFileBuffer : 128u;
        for (unsigned i = 0; i < n; ++i) {
            const TeamFileEx *t = &tmd[i];
            if (PACK_BE_U16(t->team.countryNumber,t->team.teamOrdinal) == club->id &&
                SWOS_AI_P3ValidLineup(t->team.playerNumbers)) {
                chosen = t;
                source = "TMD";
                break;
            }
        }
    }
    if (chosen) memcpy(club->lineupSlots, chosen->team.playerNumbers, 16);
    club->lineupValid = SWOS_AI_P3ValidLineup(club->lineupSlots);
    if (club->id == 0x1400u || (chosen &&
        memcmp(chosen->team.playerNumbers, club->lineupSlots,16))) {
        SWOS_AI_LOG("[SWOS AI LINEUP] ORDER club=%s [%04X] source=%s valid=%u starters=%u,%u,%u bench=%u,%u,%u,%u,%u\n",
            club->name,club->id,source,(unsigned)club->lineupValid,
            club->lineupSlots[8],club->lineupSlots[9],club->lineupSlots[10],
            club->lineupSlots[11],club->lineupSlots[12],club->lineupSlots[13],
            club->lineupSlots[14],club->lineupSlots[15]);
    }
}

static unsigned SWOS_AI_P3Collect(SWOS_AI_P3Club *clubs)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    const size_t bufferSize = 2u + 93u * sizeof(TeamFileEx);
    uint8_t *snapshot = malloc(bufferSize);
    if (!snapshot) {
        SWOS_AI_LOG("[SWOS AI WORLD D3] ERROR: snapshot allocation failed\n");
        return 0;
    }
    unsigned clubCount = 0;
    for (unsigned country = 0; country <= 79 && clubCount < SWOS_AI_P3_MAX_CLUBS; country++) {
        if (SWOS_AI_P3MissingCountry(country)) {
            SWOS_AI_LOG("[SWOS AI WORLD D3] SKIP excluded country=%u\n", country);
            continue;
        }
        const char *skip = getenv("SWOS_AI_WORLD_SKIP_COUNTRY");
        if (skip && *skip) {
            char *endptr = NULL;
            long skipId = strtol(skip, &endptr, 10);
            if (endptr != skip && *endptr == '\0' && skipId == (long)country) {
                SWOS_AI_LOG("[SWOS AI WORLD D3] skip requested country=%u\n", country);
                continue;
            }
        }
        uint16_t count = 0;
        SWOS_AI_LOG("[SWOS AI WORLD D3] BEFORE read country=%u\n", country);
        if (!SWOS_AI_P3ReadCountry(country, snapshot, bufferSize, &count)) continue;
        SWOS_AI_LOG("[SWOS AI WORLD D3] AFTER read country=%u count=%u\n", country, count);
        const TeamFileEx *teams = (const TeamFileEx *)(snapshot + 2);
        for (unsigned i = 0; i < count && clubCount < SWOS_AI_P3_MAX_CLUBS; i++) {
            const TeamFileEx *team = &teams[i];
            if (team->team.countryNumber != country) continue;
            uint16_t id = (uint16_t)((country << 8) | team->team.teamOrdinal);
            /* The manager's club is excluded from AI transfers, but it
             * still needs its canonical TEAM source registered for the
             * first-team age and replacement-identity lookup. */
            if (id == _selTeamNumber) {
                SWOS_IdentityRegisterClub(id,country,i);
                continue;
            }
            SWOS_AI_P3Club *dst = &clubs[clubCount++];
            dst->id = id;
            dst->sourceFile = (uint8_t)country;
            dst->sourceRecord = (uint8_t)i;
            SWOS_IdentityRegisterClub(id,country,i);
            dst->country = (uint8_t)country;
            dst->league = team->team.league;
            memcpy(dst->name, team->team.teamName, 17);
            dst->name[17] = 0;
            memcpy(dst->players, team->players, sizeof(dst->players));
            memcpy(dst->lineupSlots, team->team.playerNumbers,
                   sizeof(dst->lineupSlots));
            /* Reuse the career's actual displayed lineup, not the original
             * TEAM file permutation that may have changed during play. */
            SWOS_AI_P3CopyCareerLineup(dst);
            for (unsigned k = 0; k < _numForeignMarketPlayers2; k++) {
                const MarketPlayer *p = &_careerForeignMarketPlayers[k];
                const uint8_t *bytes = (const uint8_t *)&p->teamId;
                if (bytes[0] == (uint8_t)(id >> 8) &&
                    bytes[1] == (uint8_t)id &&
                    p->playerOrdinal >= 0 && p->playerOrdinal < 16)
                    dst->players[(unsigned)p->playerOrdinal] = p->player;
            }
        }
        SWOS_AI_LOG("[SWOS AI WORLD D3] FINISHED country=%u accumulated-clubs=%u\n", country, clubCount);
    }
    free(snapshot);
    SWOS_AI_LOG("[SWOS AI WORLD D3] BEFORE sorting clubs=%u\n", clubCount);
    qsort(clubs, clubCount, sizeof(*clubs), SWOS_AI_P3Cmp);
    SWOS_AI_LOG("[SWOS AI WORLD D3] AFTER sorting clubs=%u\n", clubCount);
    return clubCount;
}

/* A human-to-AI sale uses the ordinary transfer market rather than the
 * seasonal AI-to-AI exchange pass. Optimise that one receiving club after
 * the sale, using the exact same safe player/identity/market-pool swap code
 * as the seasonal AI optimiser. Read its original TEAM record separately;
 * overlay the current career pool so the newly purchased player is present.
 * No global world transfer run or new career state is required. */
void SWOS_AI_OptimiseClubAfterHumanSale(unsigned teamId)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    if (teamId == (unsigned)_selTeamNumber || teamId > 0x4FFFu ||
        SWOS_AI_P3MissingCountry(teamId >> 8)) return;
    unsigned country = teamId >> 8;
    const size_t size = 2u + 93u * sizeof(TeamFileEx);
    uint8_t *raw = malloc(size);
    if (!raw) {
        SWOS_AI_LOG("[SWOS AI LINEUP] HUMAN_SALE_SKIP club=%04X reason=allocation\n", teamId);
        return;
    }
    uint16_t count = 0;
    if (!SWOS_AI_P3ReadCountry(country, raw, size, &count)) {
        SWOS_AI_LOG("[SWOS AI LINEUP] HUMAN_SALE_SKIP club=%04X reason=team_file\n", teamId);
        free(raw);
        return;
    }
    SWOS_AI_P3Club *club = calloc(1, sizeof(*club));
    if (!club) { free(raw); return; }
    const TeamFileEx *teams = (const TeamFileEx *)(raw + 2);
    int found = 0;
    for (unsigned i = 0; i < count; ++i) {
        const TeamFileEx *team = &teams[i];
        if (team->team.countryNumber != country ||
            team->team.teamOrdinal != (uint8_t)teamId) continue;
        club->id = (uint16_t)teamId;
        club->sourceFile = (uint8_t)country;
        club->sourceRecord = (uint8_t)i;
        club->country = (uint8_t)country;
        club->league = team->team.league;
        memcpy(club->name, team->team.teamName, 17);
        club->name[17] = 0;
        memcpy(club->players, team->players, sizeof(club->players));
        memcpy(club->lineupSlots, team->team.playerNumbers, sizeof(club->lineupSlots));
        SWOS_IdentityRegisterClub(teamId, country, i);
        SWOS_AI_P3CopyCareerLineup(club);
        for (unsigned k = 0; k < _numForeignMarketPlayers2; ++k) {
            const MarketPlayer *entry = &_careerForeignMarketPlayers[k];
            if (SWOS_AI_P3OverrideMatches(entry, (uint16_t)teamId,
                                           (unsigned)(uint8_t)entry->playerOrdinal) &&
                entry->playerOrdinal >= 0 && entry->playerOrdinal < 16)
                club->players[(unsigned)entry->playerOrdinal] = entry->player;
        }
        found = 1;
        break;
    }
    free(raw);
    if (found && club->lineupValid) {
        unsigned promoted = SWOS_AI_P3OptimiseLineup(club);
        SWOS_AI_LOG("[SWOS AI LINEUP] HUMAN_SALE club=%04X promotions=%u valid=1\n",
                    teamId, promoted);
        swos_age_log("HUMAN_SALE_LINEUP", "club=%04X promotions=%u",
                     teamId, promoted);
    } else {
        SWOS_AI_LOG("[SWOS AI LINEUP] HUMAN_SALE_SKIP club=%04X reason=%s\n",
                    teamId, found ? "invalid_lineup" : "club_not_found");
    }
    free(club);
}

/* An exact position and currency-value match must always hold. Requested
 * direction applies to the initiating club; the counterparty necessarily
 * receives the opposite value effect in a two-player swap. */
/* Step 4B.2: opt-in live retirement on AI clubs with canonical, untouched
 * roster slots only. This uses the same market override writer and original
 * identity registry as AI transfers, so save/load requires no new CAR fields.
 * The managed 32-slot squad and manual overrides remain excluded in this
 * first live test; neither is safe to mutate through the AI override path. */
/* Form a new footballer's name from real SWOS players of the same
 * nationality. A first name and a surname come from independently sampled
 * donors. The original database and career overrides are never modified.
 * Avoid duplicates against the current AI world, including new youths. */
static int SWOS_RetirementYouthName(char out[23], const SWOS_AI_P3Club *clubs,
                                    unsigned clubCount, unsigned nationality,
                                    uint32_t *seed)
{
    for(unsigned attempt=0; attempt<24u; attempt++){
        const PlayerFile *first=NULL,*last=NULL;
        unsigned seenFirst=0,seenLast=0;
        for(unsigned c=0;c<clubCount;c++)for(unsigned s=0;s<16u;s++){
            const PlayerFile *p=&clubs[c].players[s];
            if((unsigned)(uint8_t)p->nationality!=nationality ||
               !SWOS_AI_P3IsValid(*p) || !p->playerName[0])continue;
            const char *name=(const char *)p->playerName;
            if(strncmp(name,"YOUTH ",6)==0)continue;
            if(SWOS_RetirementRandom(seed)%++seenFirst==0)first=p;
            if(SWOS_RetirementRandom(seed)%++seenLast==0)last=p;
        }
        if(!first||!last)break;
        char a[23]={0},b[23]={0},candidate[23]={0};
        memcpy(a,first->playerName,22);memcpy(b,last->playerName,22);
        /* Many SWOS records have only a surname. For those, use a surname
         * paired with a different donor rather than inventing a nationality. */
        char *space=strchr(a,' ');
        if(space)*space=0;
        const char *surname=strrchr(b,' ');
        surname=surname&&surname[1]?surname+1:b;
        if(!a[0]||!surname[0]||!strcmp(a,surname))continue;
        snprintf(candidate,sizeof(candidate),"%.10s %.11s",a,surname);
        if(!candidate[0])continue;
        int duplicate=0;
        for(unsigned c=0;c<clubCount&&!duplicate;c++)for(unsigned s=0;s<16u;s++)
            if(strncmp((const char *)clubs[c].players[s].playerName,candidate,22)==0){
                duplicate=1;break;
            }
        if(duplicate)continue;
        memcpy(out,candidate,23);
        return 1;
    }
    return 0;
}
static PlayerFile SWOS_RetirementMakeYouth(const SWOS_AI_P3Club *club,
                                          unsigned slot, unsigned id,
                                          uint32_t *seed)
{
    PlayerFile p=club->players[slot];
    unsigned pos=SWOS_AI_P3Position(p), peers=0, sums[7]={0}, priceSum=0;
    for(unsigned i=0;i<16;i++){
        if(i==slot)continue;
        const PlayerFile *q=&club->players[i];
        if(!SWOS_AI_P3IsValid(*q)||SWOS_AI_P3Position(*q)!=pos)continue;
        uint8_t a[7]={
            (uint8_t)q->passing&15u,
            ((uint8_t)q->velocityHeading>>4)&15u,
            (uint8_t)q->velocityHeading&15u,
            ((uint8_t)q->tacklingControl>>4)&15u,
            (uint8_t)q->tacklingControl&15u,
            ((uint8_t)q->speedFinishing>>4)&15u,
            (uint8_t)q->speedFinishing&15u};
        for(unsigned k=0;k<7;k++)sums[k]+=a[k]>7?7:a[k];
        priceSum+=(uint8_t)q->price;
        peers++;
    }
    /* Preserve position and nationality exactly. Reset card, injury, goal
     * stats and match state. Use a unique temporary youth name to prevent
     * the name-indexed fallback from accidentally matching the retiree. */
    memset(p.playerName,0,sizeof(p.playerName));
    snprintf((char *)p.playerName,sizeof(p.playerName),"YOUTH %u",id);
    memset(p.goalStats,0,sizeof(p.goalStats));
    p.cardsInjuries=0;
    p.form=0;
    p.cachedMatchValue=0;
    p.positionAndFace&=(uint8_t)~(PF_LOAN|PF_TRIALIST|PF_INFORM);
    /* Retirement balance v2: replacement quality follows the footballer
     * who retired, rather than converging towards a common club average.
     * A small (at most two points) same-position peer adjustment keeps the
     * youth connected to the club's level without erasing elite footballers.
     * The output is always within the tier's percentage limits relative to
     * the retiree. Goalkeepers use price as their quality proxy; outfield
     * players use their seven effective (0..7) skills and derive price from
     * those skills, as before. */
    unsigned averageValue=peers?(priceSum+peers/2u)/peers:15u;
    unsigned youthValue;
    unsigned actualSkill=0u,averageSkill=0u;
    unsigned retiredSkill=0u, baseline, lowPct, highPct, abilityPct;
    unsigned target, tier, peerBenchmark;
    if(pos!=0){
        const uint8_t retiredAttrs[7]={
            (uint8_t)p.passing&7u,
            ((uint8_t)p.velocityHeading>>4)&7u,
            (uint8_t)p.velocityHeading&7u,
            ((uint8_t)p.tacklingControl>>4)&7u,
            (uint8_t)p.tacklingControl&7u,
            ((uint8_t)p.speedFinishing>>4)&7u,
            (uint8_t)p.speedFinishing&7u};
        for(unsigned k=0;k<7;k++)retiredSkill+=retiredAttrs[k];
        unsigned peerTotal=0;
        for(unsigned k=0;k<7;k++)peerTotal+=sums[k];
        averageSkill=peers?(peerTotal+peers/2u)/peers:retiredSkill;
    }
    baseline=pos==0?(unsigned)(uint8_t)p.price:retiredSkill;
    peerBenchmark=pos==0?(peers?averageValue:baseline):averageSkill;
    if(baseline<10u){lowPct=80u;highPct=120u;tier=0u;}
    else if(baseline<20u){lowPct=85u;highPct=115u;tier=1u;}
    else if(baseline<30u){lowPct=85u;highPct=110u;tier=2u;}
    else if(baseline<40u){lowPct=90u;highPct=110u;tier=3u;}
    else {lowPct=90u;highPct=105u;tier=4u;}
    abilityPct=lowPct+SWOS_RetirementRandom(seed)%(highPct-lowPct+1u);
    /* Round the target against the retiring player first, then allow a
     * maximum two-point peer adjustment without leaving the tier range. */
    target=(baseline*abilityPct+50u)/100u;
    if(peers){
        int difference=(int)peerBenchmark-(int)baseline;
        int adjustment=difference/10;
        if(adjustment>2)adjustment=2;
        if(adjustment< -2)adjustment= -2;
        int adjusted=(int)target+adjustment;
        unsigned lower=(baseline*lowPct+50u)/100u;
        unsigned upper=(baseline*highPct+50u)/100u;
        if(adjusted<(int)lower)adjusted=(int)lower;
        if(adjusted>(int)upper)adjusted=(int)upper;
        target=(unsigned)adjusted;
    }
    if(pos==0){
        if(target<1u)target=1u;
        if(target>127u)target=127u;
        youthValue=target;
        p.price=(int8_t)youthValue;
        p.passing=p.velocityHeading=p.tacklingControl=p.speedFinishing=0;
    }else{
        unsigned attrs[7]={0};
        if(target<1u)target=1u;
        if(target>49u)target=49u;
        /* Distribute the total according to same-position peer profiles.
         * Preserve bounded 0..7 attributes and their position-specific shape. */
        for(unsigned n=0;n<target;n++){
            unsigned best=0,score=0;
            for(unsigned k=0;k<7;k++){
                if(attrs[k]>=7u)continue;
                unsigned preference=peers?sums[k]*8u/peers:3u*8u;
                unsigned weighted=preference+SWOS_RetirementRandom(seed)%17u;
                weighted=weighted>attrs[k]*8u?weighted-attrs[k]*8u:0u;
                if(weighted>=score){score=weighted;best=k;}
            }
            if(attrs[best]<7u)attrs[best]++;
        }
        for(unsigned k=0;k<7;k++)actualSkill+=attrs[k];
        p.passing=(int8_t)attrs[0];
        p.velocityHeading=(int8_t)((attrs[1]<<4)|attrs[2]);
        p.tacklingControl=(int8_t)((attrs[3]<<4)|attrs[4]);
        p.speedFinishing=(int8_t)((attrs[5]<<4)|attrs[6]);
        youthValue=actualSkill;
        if(youthValue<1u)youthValue=1u;
        p.price=(int8_t)youthValue;
    }
    swos_age_log("RETIRE_YOUTH_BALANCE",
        "club=%04X slot=%u new_id=%u position=%u peers=%u peer_price=%u youth_price=%u retired_quality=%u quality_tier=%u ability_pct=%u peer_skill=%u youth_skill=%u",
        (unsigned)club->id,slot,id,pos,peers,averageValue,youthValue,baseline,tier,abilityPct,averageSkill,actualSkill);
    return p;
}
/* Step 4M: diagnostic-only, append-only retirement/replacement value audit.
 * Values are raw SWOS price units (not cash); skills are seven 0..7 ratings.
 * A goalkeeper has no comparable outfield skill total, so use -1.
 * The CSV includes permanent identities so a transfer can be distinguished
 * from a genuinely new footballer. No CAR schema changes are needed. */
typedef struct {
    unsigned season, count, aiCount, managedCount;
    unsigned long long oldValue, newValue, oldSkill, newSkill;
    unsigned skillPairs;
} SWOS_RetirementBalanceSummary;
static SWOS_RetirementBalanceSummary s_retirementBalance;

static int SWOS_RetirementBalanceSkill(const PlayerFile *p)
{
    if (SWOS_AI_P3Position(*p) == 0) return -1;
    return ((uint8_t)p->passing & 7) +
           (((uint8_t)p->velocityHeading >> 4) & 7) +
           ((uint8_t)p->velocityHeading & 7) +
           (((uint8_t)p->tacklingControl >> 4) & 7) +
           ((uint8_t)p->tacklingControl & 7) +
           (((uint8_t)p->speedFinishing >> 4) & 7) +
           ((uint8_t)p->speedFinishing & 7);
}

static void SWOS_RetirementBalanceCsvName(FILE *f, const PlayerFile *p)
{
    fputc('"', f);
    for (unsigned i = 0; i < sizeof(p->playerName) && p->playerName[i]; i++) {
        unsigned char ch = (unsigned char)p->playerName[i];
        if (ch == '"') fputc('"', f);
        if (ch >= 32 && ch < 127) fputc(ch, f);
    }
    fputc('"', f);
}

static void SWOS_RetirementBalanceRecord(unsigned season, int managed,
    unsigned club, unsigned slot, unsigned oldId, unsigned newId,
    unsigned oldAge, const PlayerFile *oldPlayer, const PlayerFile *newPlayer)
{
    const unsigned before = (unsigned)(uint8_t)oldPlayer->price;
    const unsigned after = (unsigned)(uint8_t)newPlayer->price;
    const int oldSkill = SWOS_RetirementBalanceSkill(oldPlayer);
    const int newSkill = SWOS_RetirementBalanceSkill(newPlayer);
    FILE *f = fopen("swos_retirement_balance.csv", "a+");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0)
            fputs("season,team_type,club_id,slot,position,nationality,retired_id,retired_name,retired_age,retired_value,retired_skill,new_id,new_name,new_age,new_value,new_skill,value_ratio_pct,skill_ratio_pct\n", f);
        fprintf(f, "%u,%s,%04X,%u,%u,%u,%u,", season,
                managed ? "MANAGED" : "AI", club, slot,
                SWOS_AI_P3Position(*oldPlayer),
                (unsigned)(uint8_t)oldPlayer->nationality, oldId);
        SWOS_RetirementBalanceCsvName(f, oldPlayer);
        fprintf(f, ",%u,%u,%d,%u,", oldAge, before, oldSkill, newId);
        SWOS_RetirementBalanceCsvName(f, newPlayer);
        fprintf(f, ",17,%u,%d,", after, newSkill);
        if (before) fprintf(f, "%u", (after * 100u + before / 2u) / before);
        fputc(',', f);
        if (oldSkill > 0 && newSkill >= 0)
            fprintf(f, "%u", ((unsigned)newSkill * 100u + (unsigned)oldSkill / 2u) / (unsigned)oldSkill);
        fputc('\n', f);
        fclose(f);
    }
    if (s_retirementBalance.count == 0 || s_retirementBalance.season != season) {
        memset(&s_retirementBalance, 0, sizeof s_retirementBalance);
        s_retirementBalance.season = season;
    }
    s_retirementBalance.count++;
    if (managed) s_retirementBalance.managedCount++;
    else s_retirementBalance.aiCount++;
    s_retirementBalance.oldValue += before;
    s_retirementBalance.newValue += after;
    if (oldSkill >= 0 && newSkill >= 0) {
        s_retirementBalance.oldSkill += (unsigned)oldSkill;
        s_retirementBalance.newSkill += (unsigned)newSkill;
        s_retirementBalance.skillPairs++;
    }
}

static void SWOS_RetirementBalanceFlush(unsigned season)
{
    const SWOS_RetirementBalanceSummary *r = &s_retirementBalance;
    if (!r->count || r->season != season) return;
    FILE *f = fopen("swos_retirement_balance_seasons.csv", "a+");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0)
            fputs("season,retirements,ai,managed,retired_total_value,new_total_value,retired_mean_value,new_mean_value,value_ratio_pct,outfield_pairs,retired_mean_skill,new_mean_skill,skill_ratio_pct\n", f);
        fprintf(f, "%u,%u,%u,%u,%llu,%llu,%.3f,%.3f,",
                season,r->count,r->aiCount,r->managedCount,
                r->oldValue,r->newValue,
                (double)r->oldValue/r->count,(double)r->newValue/r->count);
        if (r->oldValue) fprintf(f,"%.2f",100.0*(double)r->newValue/r->oldValue);
        fprintf(f,",%u,",r->skillPairs);
        if (r->skillPairs) {
            fprintf(f,"%.3f,%.3f,",(double)r->oldSkill/r->skillPairs,
                    (double)r->newSkill/r->skillPairs);
            if (r->oldSkill) fprintf(f,"%.2f",100.0*(double)r->newSkill/r->oldSkill);
        } else fputs(",,",f);
        fputc('\n',f);
        fclose(f);
    }
    swos_age_log("RETIRE_BALANCE_SEASON", "season=%u events=%u value_before=%llu value_after=%llu skill_pairs=%u skill_before=%llu skill_after=%llu",
                 season,r->count,r->oldValue,r->newValue,r->skillPairs,r->oldSkill,r->newSkill);
    memset(&s_retirementBalance,0,sizeof s_retirementBalance);
}

/* The retirement news menu collects managed-club departures before the
 * retiring player is replaced. The UI is shown only after the season update. */
extern void SWOS_RetirementNewsClear(unsigned season);
extern void SWOS_RetirementNewsRecord(const PlayerFile *player, unsigned age);
extern int SWOS_RetirementNewsShow(void);

/* Reconcile an occupied slot for which all persistent identity lookups fail.
 * We only attach a new identity when the saved name-age table gives an exact
 * age and that name is unique inside this club. Never resurrect a tombstoned
 * canonical ID or infer an original identity from its physical slot. */
static int SWOS_RetirementRecoverUntracked(unsigned club, unsigned slot,
                                           const PlayerFile *player,
                                           const PlayerFile *roster,
                                           unsigned rosterCount,
                                           unsigned season,
                                           const char *reason)
{
    extern int SWOS_AgesLookupExact(const char *name, unsigned *age);
    if (!player || !roster || !SWOS_AI_P3IsValid(*player) ||
        slot >= rosterCount || s_identity_next_id >= 4000000u ||
        SWOS_ManualFind(club, slot) >= 0 &&
        s_manual_ids[SWOS_ManualFind(club, slot)].id ||
        !s_identity_records) return 0;
    char name[sizeof(player->playerName) + 1u];
    memcpy(name, player->playerName, sizeof(player->playerName));
    name[sizeof(player->playerName)] = 0;
    size_t len = strlen(name);
    while (len && name[len - 1u] == ' ') name[--len] = 0;
    unsigned age = 0;
    if (!len || !SWOS_AgesLookupExact(name, &age) || age < 15u || age > 100u) {
        swos_age_log("RETIRE_UNTRACKED_UNRESOLVED",
            "season=%u club=%04X slot=%u reason=%s name=%.22s age_lookup=0",
            season, club, slot, reason, name);
        return 0;
    }
    for (unsigned i=0; i<rosterCount; ++i)
        if (i != slot && SWOS_AI_P3IsValid(roster[i]) &&
            memcmp(roster[i].playerName, player->playerName,
                   sizeof(player->playerName)) == 0) {
            swos_age_log("RETIRE_UNTRACKED_UNRESOLVED",
                "season=%u club=%04X slot=%u reason=duplicate_name name=%.22s age=%u",
                season, club, slot, name, age);
            return 0;
        }
    const unsigned newId = s_identity_next_id;
    if (!SWOS_ManualPut(club, slot, newId, age, 0u)) return 0;
    ++s_identity_next_id;
    swos_age_log("RETIRE_UNTRACKED_RECOVERED",
        "season=%u club=%04X slot=%u id=%u age=%u reason=%s name=%.22s",
        season, club, slot, newId, age, reason, name);
    return 1;
}

/* Step 4C.2: Managed club original 16-slot retirement, separately opt-in.
 * Never infer identities for a previously transferred or unverified slot.
 * The owned career squad is persisted by the normal CAR serializer. */
static void SWOS_RetirementLiveManaged(unsigned season,
                                       SWOS_AI_P3Club *world,unsigned worldCount,
                                       uint32_t *seed)
{
    const char *flag=getenv("SWOS_RETIREMENT_LIVE_MANAGED");
    if(flag && strcmp(flag,"1")!=0)return;
    const unsigned team=_selTeamNumber;
    if(_isNationalSelector || !s_identity_records || team==0xFFFFu){
        swos_age_log("RETIRE_LIVE_MANAGED","ready=0 team=%04X reason=national_or_missing_registry",team);
        return;
    }
    if(!s_identity_club_sources[team]) SWOS_IdentityResolveClub(team);
    unsigned source=s_identity_club_sources[team];
    if(!source)
        swos_age_log("RETIRE_LIVE_MANAGED","team=%04X reason=unverified_original_mapping manual_slots_only=1",team);
    unsigned file=source?(source-1u)>>8:0u;
    unsigned record=source?(source-1u)&255u:0u;
    SWOS_AI_P3Club local={0};
    local.id=(uint16_t)team;
    memcpy(local.players,_careerTeam.players,sizeof(local.players));
    unsigned inspected=0,eligible=0,retired=0,skipped=0,transferred=0,generatedRetired=0;
    for(unsigned slot=0;slot<32u;slot++){
        int tracked=SWOS_ManualIdentityIsTouched(team,slot);
        /* A generated identity is authoritative at its physical 0..31 slot.
         * Do not confuse it with the canonical shadow record, which can still
         * describe the original player displaced by RES/TRIAL generation. */
        int generatedIndex=-1;
        for(unsigned g=0;g<s_generated_count;g++)
            if(s_generated[g].team==team&&s_generated[g].slot==slot){
                generatedIndex=(int)g;break;
            }
        unsigned age=0,oldId=0;
        int ix=-1;
        if(tracked && generatedIndex>=0 &&
           SWOS_ManualFind(team,slot)>=0 &&
           s_manual_ids[SWOS_ManualFind(team,slot)].id){
            /* Two ACTIVE registries claim one player. An obsolete zero-ID
             * tombstone is not a competing claim and must not suppress a
             * perfectly valid generated youth's retirement eligibility. */
            skipped++;
            swos_age_log("RETIRE_LIVE_MANAGED_SKIP",
                "club=%04X slot=%u reason=active_manual_generated_overlap",team,slot);
            continue;
        }
        if(generatedIndex>=0){
            age=s_generated[generatedIndex].age;
            oldId=s_generated[generatedIndex].id;
        }else if(tracked){
            /* A tombstone means the player moved away, not an identity. */
            if(!SWOS_ManualIdentityAge(team,slot,&age,&oldId)){
                if (SWOS_RetirementRecoverUntracked(team,slot,
                    &_careerTeam.players[slot],_careerTeam.players,32u,
                    season,"managed_tombstone") &&
                    SWOS_ManualIdentityAge(team,slot,&age,&oldId)) {
                    /* Recovered a current player, not the retired original. */
                } else {
                    skipped++;
                    swos_age_log("RETIRE_MANAGED_UNTRACKED",
                        "season=%u club=%04X slot=%u reason=tombstone name=%.22s",
                        season,team,slot,_careerTeam.players[slot].playerName);
                    continue;
                }
            }
        }else{
            /* Untracked reserves cannot be assigned an original identity
             * from squad position alone. */
            if(slot>=16u||!source){
                if (SWOS_RetirementRecoverUntracked(team,slot,
                    &_careerTeam.players[slot],_careerTeam.players,32u,
                    season,"managed_no_source")) {
                    if (SWOS_ManualIdentityAge(team,slot,&age,&oldId)) {
                        tracked = 1;
                        goto managed_identity_ready;
                    }
                }
                skipped++;
                swos_age_log("RETIRE_MANAGED_UNTRACKED",
                    "season=%u club=%04X slot=%u reason=no_source name=%.22s",
                    season,team,slot,_careerTeam.players[slot].playerName);
                continue;
            }
            ix=SWOS_IdentityFind(((uint32_t)file<<12)|((uint32_t)record<<4)|slot);
            if(ix<0){
                skipped++;
                swos_age_log("RETIRE_MANAGED_UNTRACKED",
                    "season=%u club=%04X slot=%u reason=no_canonical name=%.22s",
                    season,team,slot,_careerTeam.players[slot].playerName);
                continue;
            }
            oldId=s_identity_records[ix].id;
            age=s_identity_records[ix].age;
            int mirror=0;
            for(unsigned m=0;m<s_manual_count;m++)
                if(s_manual_ids[m].id==oldId){mirror=1;break;}
            if(mirror){skipped++;continue;}
        }
managed_identity_ready:
        if(!SWOS_AI_P3IsValid(_careerTeam.players[slot])){
            skipped++;continue;
        }
        inspected++;
        unsigned chance=SWOS_RetirementChance(age);
        if (age>=40u)
            swos_age_log("RETIRE_40PLUS_MANAGED_AUDIT",
                "season=%u club=%04X slot=%u id=%u age=%u eligible=%u name=%.22s",
                season,team,slot,oldId,age,(unsigned)(chance>0),
                _careerTeam.players[slot].playerName);
        if(!chance)continue;
        eligible++;
        unsigned roll=SWOS_RetirementRandom(seed)%100u;
        if(roll>=chance)continue;
        if(s_identity_next_id>=4000000u){skipped++;continue;}
        /* Youth generation uses up to 15 peer records. For reserve slots,
         * stage the retiring record in a temporary 16-player snapshot. */
        SWOS_AI_P3Club snapshot=local;
        unsigned makerSlot=slot;
        if(slot>=16u){
            makerSlot=15u;
            snapshot.players[makerSlot]=_careerTeam.players[slot];
        }
        PlayerFile retiringPlayer=_careerTeam.players[slot];
        PlayerFile youth=SWOS_RetirementMakeYouth(&snapshot,makerSlot,
                                                   s_identity_next_id,seed);
        char youthName[23]={0};
        if(SWOS_RetirementYouthName(youthName,world,worldCount,
                                     (unsigned)(uint8_t)youth.nationality,seed))
            memcpy(youth.playerName,youthName,sizeof(youth.playerName));
        unsigned prior=0;
        unsigned fresh=0;
        if(generatedIndex>=0){
            /* GeneratedIdentityCreate reassigns the existing slot a fresh,
             * monotonic ID and leaves it in the persisted generated registry. */
            prior=oldId;
            fresh=SWOS_GeneratedIdentityCreate(team,slot,17u);
        }else if(tracked)
            fresh=SWOS_ManualIdentityReplace(team,slot,17u,&prior);
        else
            fresh=SWOS_IdentityReplaceCanonical(file,record,slot,17u,&prior);
        if(!fresh){skipped++;continue;}
        _careerTeam.players[slot]=youth;
        /* The squad UI has a name-based age fallback when a physical-slot
         * identity cannot be resolved. Register the new youth at age 17 as
         * well as assigning its persistent ID, so it never falls back to 25. */
        {
            extern int SWOS_AgesSet(const char *name, unsigned age);
            char youthDisplayName[sizeof(youth.playerName) + 1u];
            memcpy(youthDisplayName, youth.playerName, sizeof(youth.playerName));
            youthDisplayName[sizeof(youth.playerName)] = 0;
            size_t youthNameLength = strlen(youthDisplayName);
            while (youthNameLength && youthDisplayName[youthNameLength - 1u] == ' ')
                youthDisplayName[--youthNameLength] = 0;
            if (youthNameLength)
                SWOS_AgesSet(youthDisplayName, 17u);
        }
        if(slot<16u)local.players[slot]=youth;
        {
            extern uint16_t _currentTeamNumber;
            extern uint8_t _currentTeam[];
            if(_currentTeamNumber==team)
                ((TeamFileEx *)_currentTeam)->players[slot]=youth;
        }
        if(!SWOS_RetiredIdentityAppend(prior,season,age,team,
                                       (unsigned)(uint8_t)youth.nationality))
            swos_age_log("RETIRE_HISTORY_FAIL","id=%u club=%04X",prior,team);
        SWOS_RetirementBalanceRecord(season,1,team,slot,prior,fresh,age,
                                    &retiringPlayer,&youth);
        SWOS_RetirementNewsRecord(&retiringPlayer, age);
        retired++;
        if(tracked)transferred++;
        if(generatedIndex>=0)generatedRetired++;
        swos_age_log("RETIRE_LIVE_MANAGED_REPLACED",
            "club=%04X slot=%u old_id=%u new_id=%u age=%u nationality=%u position=%u transferred=%u generated=%u",
            team,slot,prior,fresh,age,(unsigned)(uint8_t)youth.nationality,
            SWOS_AI_P3Position(youth),(unsigned)tracked,(unsigned)(generatedIndex>=0));
    }
    swos_age_log("RETIRE_LIVE_MANAGED",
        "season=%u club=%04X inspected=%u eligible=%u retired=%u replacements=%u transferred=%u generated=%u skipped=%u registry_valid=%u scope=verified_32_slots",
        season,team,inspected,eligible,retired,retired,transferred,generatedRetired,skipped,SWOS_IdentityCheck());
}

static void SWOS_RetirementLiveAI(unsigned season)
{
    const char *enabled=getenv("SWOS_RETIREMENT_LIVE_AI");
    const char *managed=getenv("SWOS_RETIREMENT_LIVE_MANAGED");
    int aiOn=!enabled||strcmp(enabled,"1")==0;
    int managedOn=!managed||strcmp(managed,"1")==0;
    if(!aiOn&&!managedOn)return;
    if(!s_identity_records){swos_age_log("RETIRE_LIVE_AI","ready=0 reason=no_registry");return;}
    SWOS_AI_P3Club *clubs=calloc(SWOS_AI_P3_MAX_CLUBS,sizeof(*clubs));
    if(!clubs){swos_age_log("RETIRE_LIVE_AI","ready=0 reason=allocation");return;}
    SWOS_AI_P3ReconcileOverrides();
    SWOS_AI_P3CompactActiveOverrides();
    unsigned n=SWOS_AI_P3Collect(clubs);
    uint32_t seed=0x52455449u^(season*2654435761u);
    const char *seedStr=getenv("SWOS_RETIREMENT_SEED");
    if(seedStr&&*seedStr){char *end=NULL;unsigned long v=strtoul(seedStr,&end,0);
        if(end!=seedStr&&!*end)seed=(uint32_t)v;}
    if(!seed)seed=1;
    SWOS_RetirementLiveManaged(season,clubs,n,&seed);
    if(!aiOn){SWOS_RetirementBalanceFlush(season);free(clubs);return;}
    unsigned inspected=0,eligible=0,retired=0,skipped=0,capacity=0,generatedRetired=0,manualRetired=0,manualTombstones=0,overlaps=0;
    unsigned recoveredGenerated=0, staleTombstoneGenerated=0, missingIdentity=0, mirroredClaims=0;
    unsigned eligibleByAge[6]={0},retiredByAge[6]={0};
    for(unsigned c=0;c<n;c++){
        SWOS_AI_P3Club *club=&clubs[c];
        for(unsigned slot=0;slot<16;slot++){
            int manualIndex=SWOS_ManualFind(club->id,slot);
            int generatedIndex=-1;
            for(unsigned gi=0;gi<s_generated_count;gi++)
                if(s_generated[gi].team==club->id&&s_generated[gi].slot==slot){
                    generatedIndex=(int)gi;break;
                }
            /* A zero-ID manual entry is a tombstone, not a competing player.
             * A legitimate generated identity at that slot remains usable.
             * Keep the tombstone untouched unless this youth actually retires. */
            if(manualIndex>=0 && !s_manual_ids[manualIndex].id){
                if(generatedIndex>=0){
                    staleTombstoneGenerated++;
                    manualIndex=-1;
                }else{
                    if (SWOS_RetirementRecoverUntracked(club->id,slot,
                        &club->players[slot],club->players,16u,
                        season,"ai_tombstone")) {
                        manualIndex=SWOS_ManualFind(club->id,slot);
                    } else {
                        manualTombstones++;skipped++;
                        swos_age_log("RETIRE_AI_UNTRACKED",
                            "season=%u club=%04X slot=%u reason=tombstone name=%.22s",
                            season,club->id,slot,club->players[slot].playerName);
                        continue;
                    }
                }
            }
            if(manualIndex>=0&&generatedIndex>=0){
                overlaps++;skipped++;continue;
            }
            uint32_t key=((uint32_t)club->sourceFile<<12)|
                         ((uint32_t)club->sourceRecord<<4)|slot;
            int ix=SWOS_IdentityFind(key);
            /* Generated and manually tracked players do not need a canonical
             * source slot. Previously a missing ix excluded even a valid
             * generated footballer from ageing-based retirement forever. */
            if (manualIndex<0 && generatedIndex<0 && ix<0 &&
                SWOS_RetirementRecoverUntracked(club->id,slot,
                    &club->players[slot],club->players,16u,
                    season,"ai_missing_canonical"))
                manualIndex=SWOS_ManualFind(club->id,slot);
            if((manualIndex<0&&generatedIndex<0&&ix<0)||
               !SWOS_AI_P3IsValid(club->players[slot])){
                if(ix<0&&manualIndex<0&&generatedIndex<0)missingIdentity++;
                skipped++;continue;
            }
            unsigned trackedId=manualIndex>=0?s_manual_ids[manualIndex].id:
                generatedIndex>=0?s_generated[generatedIndex].id:s_identity_records[ix].id;
            if(generatedIndex>=0 && ix<0)recoveredGenerated++;
            if(manualIndex<0&&generatedIndex<0){
                int mirrored=0;
                for(unsigned m=0;m<s_manual_count;m++)
                    if(s_manual_ids[m].id==trackedId){mirrored=1;break;}
                if(mirrored){mirroredClaims++;skipped++;continue;}
            }
            unsigned age=manualIndex>=0?s_manual_ids[manualIndex].age:
                generatedIndex>=0?s_generated[generatedIndex].age:s_identity_records[ix].age;
            inspected++;
            unsigned chance=SWOS_RetirementChance(age);
            if (age>=40u)
                swos_age_log("RETIRE_40PLUS_AUDIT",
                    "season=%u club=%04X slot=%u id=%u age=%u eligible=%u name=%.22s",
                    season,club->id,slot,trackedId,age,(unsigned)(chance>0),
                    club->players[slot].playerName);
            if(!chance)continue;
            eligible++;
            unsigned ageBand=age>=40u?5u:age-35u;
            eligibleByAge[ageBand]++;
            unsigned roll=SWOS_RetirementRandom(&seed)%100u;
            if(roll>=chance)continue;
            unsigned override=SWOS_AI_P3OverrideIndex(club->id,slot);
            if(override==_numForeignMarketPlayers2 &&
               _numForeignMarketPlayers2>=SWOS_AI_CAREER_POOL_CAPACITY){capacity++;continue;}
            if(s_identity_next_id>=4000000u){capacity++;continue;}
            unsigned prospective=s_identity_next_id;
            PlayerFile retiringPlayer=club->players[slot];
            PlayerFile youth=SWOS_RetirementMakeYouth(club,slot,prospective,&seed);
            char youthName[23]={0};
            if(SWOS_RetirementYouthName(youthName,clubs,n,
                                      (unsigned)(uint8_t)youth.nationality,&seed)){
                memcpy(youth.playerName,youthName,sizeof(youth.playerName));
            }
            /* Commit the canonical identity first. All potential failures
             * are checked above, and the roster write is allocation-free. */
            unsigned old=0;
            unsigned fresh;
            if(manualIndex>=0){
                fresh=SWOS_ManualIdentityReplace(club->id,slot,17u,&old);
            }else if(generatedIndex>=0){
                old=trackedId;
                fresh=SWOS_GeneratedIdentityCreate(club->id,slot,17u);
            }else{
                fresh=SWOS_IdentityReplaceCanonical(club->sourceFile,
                            club->sourceRecord,slot,17u,&old);
            }
            if(!fresh){skipped++;continue;}
            SWOS_AI_P3WriteOverrideAt(club->id,slot,youth,override);
            club->players[slot]=youth;
            Career_SyncClubPlayerCopies(club->id, slot, &youth);
            if(!SWOS_RetiredIdentityAppend(old,season,age,club->id,
                                           (unsigned)(uint8_t)youth.nationality))
                swos_age_log("RETIRE_HISTORY_FAIL","id=%u club=%04X",old,club->id);
            SWOS_RetirementBalanceRecord(season,0,club->id,slot,old,fresh,age,
                                        &retiringPlayer,&youth);
            retired++;
            retiredByAge[ageBand]++;
            if(generatedIndex>=0)generatedRetired++;
            if(manualIndex>=0)manualRetired++;
            if(retired<=20u)swos_age_log("RETIRE_LIVE_REPLACED",
                "club=%04X slot=%u old_id=%u new_id=%u age=%u chance=%u roll=%u nationality=%u position=%u manual=%u generated=%u",
                club->id,slot,old,fresh,age,chance,roll,
                (unsigned)(uint8_t)youth.nationality,SWOS_AI_P3Position(youth),
                (unsigned)(manualIndex>=0),(unsigned)(generatedIndex>=0));
        }
    }
    swos_age_log("RETIRE_LIVE_AI",
        "season=%u clubs=%u inspected=%u eligible=%u retired=%u replacements=%u generated=%u manual=%u tombstones=%u overlaps=%u skipped=%u capacity_skips=%u registry_valid=%u recovered_generated=%u generated_with_tombstone=%u missing_identity=%u mirrored_claims=%u",
        season,n,inspected,eligible,retired,retired,generatedRetired,manualRetired,
        manualTombstones,overlaps,skipped,capacity,SWOS_IdentityCheck(),
        recoveredGenerated,staleTombstoneGenerated,missingIdentity,mirroredClaims);
    swos_age_log("RETIRE_ELIGIBILITY_BY_AGE",
        "season=%u age35=%u/%u age36=%u/%u age37=%u/%u age38=%u/%u age39=%u/%u age40plus=%u/%u",
        season,retiredByAge[0],eligibleByAge[0],retiredByAge[1],eligibleByAge[1],
        retiredByAge[2],eligibleByAge[2],retiredByAge[3],eligibleByAge[3],
        retiredByAge[4],eligibleByAge[4],retiredByAge[5],eligibleByAge[5]);
    swos_age_log("RETIRE_HISTORY","season=%u total=%u",season,s_retired_count);
    SWOS_RetirementBalanceFlush(season);
    free(clubs);
}

/* Step 4R: Read-only worldwide quality snapshots after season transitions.
 * This measures all available active club players, NOT just retirees.
 * Both CSV files are append-only; club IDs distinguish repeated names.
 * TEAM files are read through the existing AI world's verified reader, and
 * transfer overrides are applied by SWOS_AI_P3Collect. No CAR changes. */
typedef struct {
    unsigned players, gk, outfield, high20, high30, low10;
    unsigned long long skill, skillSquared, rawPrice, currencyValue;
    unsigned minSkill, maxSkill;
} SWOS_WorldQualityTotals;

static void SWOS_WorldQualityAdd(SWOS_WorldQualityTotals *t, const PlayerFile *p)
{
    if (!SWOS_AI_P3IsValid(*p)) return;
    t->players++;
    t->rawPrice += (unsigned)(uint8_t)p->price;
    t->currencyValue += SWOS_AI_P3Value(*p);
    if (SWOS_AI_P3Position(*p) == 0u) { t->gk++; return; }
    int skill = SWOS_RetirementBalanceSkill(p);
    if (skill < 0) return;
    unsigned u = (unsigned)skill;
    t->outfield++;
    t->skill += u;
    t->skillSquared += (unsigned long long)u * u;
    if (u >= 20u) t->high20++;
    if (u >= 30u) t->high30++;
    if (u < 10u) t->low10++;
    if (u < t->minSkill) t->minSkill = u;
    if (u > t->maxSkill) t->maxSkill = u;
}

static void SWOS_WorldQualityMerge(SWOS_WorldQualityTotals *dst,
                                   const SWOS_WorldQualityTotals *src)
{
    dst->players += src->players;
    dst->gk += src->gk;
    dst->outfield += src->outfield;
    dst->high20 += src->high20;
    dst->high30 += src->high30;
    dst->low10 += src->low10;
    dst->skill += src->skill;
    dst->skillSquared += src->skillSquared;
    dst->rawPrice += src->rawPrice;
    dst->currencyValue += src->currencyValue;
    if (src->outfield && src->minSkill < dst->minSkill) dst->minSkill = src->minSkill;
    if (src->maxSkill > dst->maxSkill) dst->maxSkill = src->maxSkill;
}

static void SWOS_WorldQualityWriteClub(FILE *f, unsigned completed,
                                        unsigned year, unsigned team,
                                        unsigned country, unsigned league,
                                        const char *teamName, const char *kind,
                                        const SWOS_WorldQualityTotals *t)
{
    if (!f) return;
    fprintf(f, "%u,%u,%04X,%u,%u,\"", completed, year, team, country, league);
    /* Sanitise fixed-width club names for valid CSV even with quotes. */
    for (unsigned i = 0; i < 18u && teamName[i]; i++) {
        unsigned char c = (unsigned char)teamName[i];
        if (c == '"') fputc('"', f);
        if (c >= 32 && c < 127) fputc(c, f);
    }
    fprintf(f, "\",%s,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%llu,%llu,%u,%u,%u,%u,%u\n",
            kind, t->players, t->outfield, t->gk,
            t->outfield ? (double)t->skill / t->outfield : 0.0,
            t->players ? (double)t->rawPrice / t->players : 0.0,
            t->players ? (double)t->currencyValue / t->players : 0.0,
            t->outfield ? (double)t->skillSquared / t->outfield : 0.0,
            t->rawPrice, t->currencyValue, t->high20, t->high30, t->low10,
            t->outfield ? t->minSkill : 0u, t->maxSkill);
}

static void SWOS_WorldQualitySnapshot(unsigned completed)
{
    const unsigned yy = (unsigned)(uint8_t)_seasonPlaying;
    const unsigned year = yy >= 96u ? 1900u + yy : 2000u + yy;
    SWOS_AI_P3Club *clubs = calloc(SWOS_AI_P3_MAX_CLUBS, sizeof *clubs);
    if (!clubs) {
        swos_age_log("WORLD_QUALITY_ERROR", "reason=allocation season=%u", completed);
        return;
    }
    unsigned n = SWOS_AI_P3Collect(clubs);
    if (!n) {
        swos_age_log("WORLD_QUALITY_ERROR", "reason=no_clubs season=%u", completed);
        free(clubs);
        return;
    }
    FILE *clubCsv = fopen("swos_world_quality_clubs.csv", "a+");
    FILE *summaryCsv = fopen("swos_world_quality_seasons.csv", "a+");
    if (clubCsv && fseek(clubCsv, 0, SEEK_END) == 0 && ftell(clubCsv) == 0)
        fputs("completed_seasons,season_year,club_id,country,league,club_name,club_type,players,outfield,goalkeepers,mean_outfield_skill,mean_price_index,mean_converted_value,mean_skill_squared,total_price_index,total_converted_value,skill_20plus,skill_30plus,skill_under10,min_outfield_skill,max_outfield_skill\n", clubCsv);
    if (summaryCsv && fseek(summaryCsv, 0, SEEK_END) == 0 && ftell(summaryCsv) == 0)
        fputs("completed_seasons,season_year,clubs,players,outfield,goalkeepers,mean_outfield_skill,mean_price_index,mean_converted_value,total_price_index,total_converted_value,skill_20plus,skill_30plus,skill_under10,min_outfield_skill,max_outfield_skill,managed_included\n", summaryCsv);
    SWOS_WorldQualityTotals world = {0};
    world.minSkill = 99u;
    for (unsigned c = 0; c < n; c++) {
        SWOS_WorldQualityTotals club = {0};
        club.minSkill = 99u;
        for (unsigned slot = 0; slot < 16u; slot++)
            SWOS_WorldQualityAdd(&club, &clubs[c].players[slot]);
        SWOS_WorldQualityMerge(&world, &club);
        SWOS_WorldQualityWriteClub(clubCsv, completed, year, clubs[c].id,
                                   clubs[c].country, clubs[c].league,
                                   clubs[c].name, "AI", &club);
    }
    unsigned includeManaged = (_selTeamNumber != 0xFFFFu && !_isNationalSelector);
    if (includeManaged) {
        SWOS_WorldQualityTotals managed = {0};
        managed.minSkill = 99u;
        unsigned slots = _careerNumPlayers;
        if (slots > 32u) slots = 32u;
        for (unsigned s = 0; s < slots; s++)
            SWOS_WorldQualityAdd(&managed, &_careerTeam.players[s]);
        SWOS_WorldQualityMerge(&world, &managed);
        char clubName[18] = {0};
        memcpy(clubName, _careerTeam.team.teamName, 17);
        SWOS_WorldQualityWriteClub(clubCsv, completed, year, _selTeamNumber,
            _careerTeam.team.countryNumber, _careerTeam.team.league,
            clubName, "MANAGED", &managed);
    }
    if (summaryCsv) {
        fprintf(summaryCsv, "%u,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%llu,%llu,%u,%u,%u,%u,%u,%u\n",
            completed, year, n + includeManaged, world.players,
            world.outfield, world.gk,
            world.outfield ? (double)world.skill / world.outfield : 0.0,
            world.players ? (double)world.rawPrice / world.players : 0.0,
            world.players ? (double)world.currencyValue / world.players : 0.0,
            world.rawPrice, world.currencyValue, world.high20,
            world.high30, world.low10,
            world.outfield ? world.minSkill : 0u, world.maxSkill,
            includeManaged);
        fclose(summaryCsv);
    }
    if (clubCsv) fclose(clubCsv);
    swos_age_log("WORLD_QUALITY_SNAPSHOT",
        "completed=%u year=%u clubs=%u players=%u outfield=%u goalkeepers=%u mean_skill_x100=%u high20=%u high30=%u managed=%u",
        completed, year, n + includeManaged, world.players, world.outfield,
        world.gk, world.outfield ? (unsigned)(world.skill * 100u / world.outfield) : 0u,
        world.high20, world.high30, includeManaged);
    free(clubs);
}

static int SWOS_AI_P3Candidate(PlayerFile outgoing, PlayerFile incoming,
                               int wantHigher)
{
    if (!SWOS_AI_P3IsValid(outgoing) || !SWOS_AI_P3IsValid(incoming)) return 0;
    if (SWOS_AI_P3Position(outgoing) != SWOS_AI_P3Position(incoming)) return 0;
    unsigned a = SWOS_AI_P3Value(outgoing), b = SWOS_AI_P3Value(incoming);
    if (b * 100u < a * 70u || b * 100u > a * 130u) return 0;
    return wantHigher ? b > a : b < a;
}

static void SWOS_AI_RealTransferTest(void)
{
    extern MarketPlayer _careerForeignMarketPlayers[];
    const uint32_t run = ++SWOS_AI_P3ClickNumber;
    SWOS_AI_LOG("[SWOS AI WORLD] CLICK=%u starting current pool=%u capacity=%u\n",
                run, (unsigned)_numForeignMarketPlayers2,
                (unsigned)SWOS_AI_CAREER_POOL_CAPACITY);
    SWOS_AI_P3Club *clubs = calloc(SWOS_AI_P3_MAX_CLUBS, sizeof(*clubs));
    if (!clubs) {
        SWOS_AI_LOG("[SWOS AI WORLD] ERROR: club snapshot allocation failed\n");
        return;
    }
    /* Each button click normally gets a fresh seed.  SWOS_AI_SEED provides
     * an explicit reproducible seed for debugging the SAME starting career. */
    const char *seedText = getenv("SWOS_AI_SEED");
    char *seedEnd = NULL;
    unsigned long fixedSeed = seedText ? strtoul(seedText, &seedEnd, 0) : 0;
    if (seedText && *seedText && seedEnd != seedText && !*seedEnd)
        SWOS_AI_P3Seed = (uint32_t)fixedSeed;
    else
        SWOS_AI_P3Seed = (uint32_t)time(NULL) ^
            (uint32_t)clock() * 2654435761u ^
            (uint32_t)_numForeignMarketPlayers2 * 2246822519u ^
            run * 3266489917u;
    if (!SWOS_AI_P3Seed) SWOS_AI_P3Seed = 0x534F5753u;
    SWOS_AI_LOG("[SWOS AI WORLD] CLICK=%u SEED=%u cooldown=2 clicks\n",
                (unsigned)run, (unsigned)SWOS_AI_P3Seed);
    SWOS_AI_P3ReconcileOverrides();
    SWOS_AI_P3CompactActiveOverrides();
    unsigned clubCount = SWOS_AI_P3Collect(clubs);
    /* Resolve permanent identities once, before any transfer changes the
     * registry's club/slot assignments.  Leave the full PlayerFile hash as a
     * fallback when an identity genuinely cannot be resolved. */
    unsigned stableCooldownIds = 0, fallbackCooldownIds = 0;
    for (unsigned c = 0; c < clubCount; ++c) {
        for (unsigned slot = 0; slot < 16u; ++slot) {
            uint64_t key = SWOS_AI_P3CooldownKey(clubs[c].id, slot,
                                                  clubs[c].players[slot]);
            clubs[c].cooldownKeys[slot] = key;
            if (key & UINT64_C(0x8000000000000000))
                ++stableCooldownIds;
            else
                ++fallbackCooldownIds;
        }
    }
    SWOS_AI_LOG("[SWOS AI WORLD] COOLDOWN_IDENTITY stable=%u fallback=%u\n",
                stableCooldownIds, fallbackCooldownIds);
    /* Optional test-only limit; without the variable the full world runs. */
    const char *clubLimit = getenv("SWOS_AI_WORLD_MAX_CLUBS");
    if (clubLimit && *clubLimit) {
        char *end = NULL;
        long n = strtol(clubLimit, &end, 10);
        if (end != clubLimit && !*end && n >= 2 && (unsigned long)n < clubCount)
            clubCount = (unsigned)n;
    }
    unsigned completed = 0, domestic = 0, international = 0;
    unsigned higher = 0, lower = 0, shortfall = 0;
    SWOS_AI_LOG("[SWOS AI WORLD] eligible AI clubs=%u\n", clubCount);
    if (clubCount < 2) { free(clubs); return; }
    unsigned countryFirst[256], countryLast[256];
    for (unsigned c = 0; c < 256; c++)
        countryFirst[c] = countryLast[c] = 0;
    for (unsigned i = 0; i < clubCount; i++) {
        unsigned c = clubs[i].country;
        if (countryFirst[c] == countryLast[c]) countryFirst[c] = i;
        countryLast[c] = i + 1;
        clubs[i].requested = (uint8_t)(4u + SWOS_AI_P3Random() % 3u);
    }

    /* Shuffle the order of initiating clubs each click.  The inventory stays
     * sorted so countryFirst/countryLast still describe valid partner ranges. */
    unsigned *order = malloc(clubCount * sizeof(*order));
    if (!order) { SWOS_AI_LOG("[SWOS AI WORLD] ERROR: order allocation failed\n"); free(clubs); return; }
    for (unsigned i = 0; i < clubCount; i++) order[i] = i;
    for (unsigned i = clubCount - 1; i > 0; i--) {
        unsigned j = SWOS_AI_P3Random() % (i + 1u);
        unsigned temp = order[i]; order[i] = order[j]; order[j] = temp;
    }
    unsigned cooldownRejected = 0;
    for (unsigned oi = 0; oi < clubCount; oi++) {
        unsigned i = order[oi];
        SWOS_AI_P3Club *a = &clubs[i];
        unsigned tries = 0;
        while (a->completed < a->requested && tries++ < 1100u) {
            int wantInternational = (SWOS_AI_P3Random() % 100u) < 15u;
            int wantHigher = (SWOS_AI_P3Random() % 100u) < 80u;
            unsigned sa = SWOS_AI_P3Random() % 16u;
            if (a->usedSlots[sa] || !SWOS_AI_P3IsValid(a->players[sa])) continue;
            if (SWOS_AI_P3RecentlyMoved(a->cooldownKeys[sa], run)) {
                ++cooldownRejected;
                continue;
            }
            /* Rotate possible partners randomly. All candidate clubs are real
             * loaded club records. No national squads, missing team IDs, or
             * human club can be selected as a partner. */
            unsigned j;
            if (wantInternational) {
                j = SWOS_AI_P3Random() % clubCount;
            } else {
                unsigned first = countryFirst[a->country];
                unsigned n = countryLast[a->country] - first;
                if (n < 2) continue;
                j = first + SWOS_AI_P3Random() % n;
            }
            if (j == i) continue;
            SWOS_AI_P3Club *b = &clubs[j];
            if (!!(b->country != a->country) != !!wantInternational ||
                b->completed >= b->requested) continue;
            unsigned startSlot = SWOS_AI_P3Random() % 16u;
            for (unsigned s = 0; s < 16; s++) {
                    unsigned sb = (startSlot + s) % 16u;
                    if (b->usedSlots[sb] ||
                        !SWOS_AI_P3Candidate(a->players[sa], b->players[sb], wantHigher))
                        continue;
                    if (SWOS_AI_P3RecentlyMoved(b->cooldownKeys[sb], run)) {
                        ++cooldownRejected;
                        continue;
                    }
                    PlayerFile outgoing = a->players[sa];
                    PlayerFile incoming = b->players[sb];
                    uint64_t outgoingKey = a->cooldownKeys[sa];
                    uint64_t incomingKey = b->cooldownKeys[sb];
                    unsigned av = SWOS_AI_P3Value(outgoing);
                    unsigned bv = SWOS_AI_P3Value(incoming);
                    if (!SWOS_AI_P3Swap(a, sa, b, sb)) {
                        SWOS_AI_LOG("[SWOS AI WORLD] STOP: replacement pool capacity reached at %u\n",
                                    (unsigned)_numForeignMarketPlayers2);
                        goto finished;
                    }
                    char nameA[24], nameB[24];
                    memcpy(nameA, outgoing.playerName, 23); nameA[23] = 0;
                    memcpy(nameB, incoming.playerName, 23); nameB[23] = 0;
                    SWOS_AI_P3MarkMoved(outgoingKey, run);
                    SWOS_AI_P3MarkMoved(incomingKey, run);
                    ++a->completed;
                    ++b->completed;
                    ++completed;
                    if (wantInternational) ++international; else ++domestic;
                    if (wantHigher) ++higher; else ++lower;
                    SWOS_AI_LOG("[SWOS AI WORLD] EXCHANGE %u %s [%04X:%u] %s value=%u -> %s [%04X:%u]; %s value=%u -> %s; %s; %s\n",
                        completed, a->name, a->id, sa, nameA, av, b->name, b->id, sb,
                        nameB, bv, a->name, wantInternational ? "INTERNATIONAL" : "DOMESTIC",
                        wantHigher ? "HIGHER-INCOMING" : "LOWER-INCOMING");
                    break;
                }
        }
        if (a->completed < a->requested) {
            shortfall += a->requested - a->completed;
            SWOS_AI_LOG("[SWOS AI WORLD] SHORTFALL club=%s [%04X] requested=%u completed=%u\n",
                        a->name, a->id, a->requested, a->completed);
        }
    }
finished:
    unsigned lineupPromotions = 0, lineupClubs = 0;
    for (unsigned i = 0; i < clubCount; ++i) {
        unsigned promotions = SWOS_AI_P3OptimiseLineup(&clubs[i]);
        if (promotions) { ++lineupClubs; lineupPromotions += promotions; }
    }
    SWOS_AI_LOG("[SWOS AI LINEUP] SUMMARY clubs_promoted=%u promotions=%u\n",
                lineupClubs, lineupPromotions);
    unsigned auditSlots=0, auditCache=0, auditFailures=0;
    for(unsigned i=0;i<clubCount;i++) {
        for(unsigned s=0;s<16;s++) {
            if(!clubs[i].usedSlots[s]) continue;
            ++auditSlots;
            auditFailures += SWOS_AI_P3AuditSlot(clubs[i].id,s,
                                                 &clubs[i].players[s],&auditCache);
        }
    }
    SWOS_AI_LOG("[SWOS AI AUDIT] SUMMARY slots=%u cache-copies=%u failures=%u %s\n",
                auditSlots,auditCache,auditFailures,auditFailures?"FAIL":"PASS");
    SWOS_AI_LOG("[SWOS AI WORLD] SUMMARY clicks=%u exchanges=%u domestic=%u international=%u higher=%u lower=%u shortfall=%u cooldown-rejections=%u audit-failures=%u\n",
                run,completed,domestic,international,higher,lower,shortfall,cooldownRejected,auditFailures);
    SWOS_AI_LOG("[SWOS AI WORLD] COOLDOWN rejected-candidates=%u\n", cooldownRejected);
    SWOS_AI_LOG("[SWOS AI WORLD] DONE exchanges=%u domestic=%u international=%u higher=%u lower=%u unfilled-initiations=%u records=%u\n",
                completed, domestic, international, higher, lower, shortfall,
                (unsigned)_numForeignMarketPlayers2);
    swos_age_log("DUP_AUDIT_TRIGGER", "phase=after_ai_transfers run=%u exchanges=%u",run,completed);
    (void)SWOS_IdentityAudit();
    free(order);
    free(clubs);
}

int ClassicCareer_DataAudit(void)
{
    static const struct {
        unsigned country, clubs, divisions;
        const char *first;
    } cases[] = {
        {9, 16, 1, "CELIK"}, {41, 24, 2, "BUDUCNOST"}, {47, 14, 1, "SILEKS"}
    };
    static uint8_t buffer[2 + 93 * sizeof(TeamFileEx)];
    extern void *_competitionsTable[];
    extern uint16_t _teamsCountryNumbers[];
    int ok = 1;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        unsigned c = cases[i].country;
        uint16_t clubs = 0;
        CompetitionLeague *league = ((void **)_competitionsTable[c])[0];
        unsigned range = _teamsCountryNumbers[c + 1] - _teamsCountryNumbers[c];
        int match = SWOS_AI_P3ReadCountry(c, buffer, sizeof buffer, &clubs);
        const TeamFileEx *team = (const TeamFileEx *)(buffer + 2);
        unsigned divisions[16] = {0};
        for (unsigned j = 0; j < clubs; ++j) {
            unsigned division = (uint8_t)team[j].team.league;
            if (division >= cases[i].divisions ||
                (uint8_t)team[j].team.countryNumber != c)
                match = 0;
            else
                ++divisions[division];
        }
        for (unsigned j = 0; j < cases[i].divisions; ++j)
            match = match && league && divisions[j] == (unsigned)league->divisions[j].teams;
        match = match && range == cases[i].clubs && clubs == cases[i].clubs &&
            league && league->header.divisionsCount == cases[i].divisions &&
            !strncmp((const char *)team->team.teamName, cases[i].first, strlen(cases[i].first));
        fprintf(stderr, "[classic-data] TEAM.%03u: %u clubs, %u divisions: %s\n",
                c, (unsigned)clubs, cases[i].divisions, match ? "PASS" : "FAIL");
        ok &= match;
    }
    extern int SWOS_AgesLookupExact(const char *, unsigned *);
    unsigned age = 0;
    int ageOk = SWOS_AgesLookupExact("A ALEVEDO", &age) && age == 23;
    int namesOk = swos_data_exists("data/national_fake_names.csv");
    fprintf(stderr, "[classic-data] historical age lookup: %s; national names: %s\n",
            ageOk ? "PASS" : "FAIL", namesOk ? "PASS" : "FAIL");
    return ok && ageOk && namesOk ? 0 : 6;
}

void ClassicCareer_ResetPlayers(void)
{
    extern void ClassicNational_Reset(void);
    ClassicNational_Reset();
    SWOS_AgesResetCareer();
    SWOS_IdentityClear();
    SWOS_AI_IdentityJournalClear();
    SWOS_AI_P3ResetHistory();
    if (SWOS_IdentitySeed())
        SWOS_IdentityApplyCsvOverrides();
}

typedef enum {
    CLUB_RETIREMENT, NATIONAL_RETIREMENT, NATIONAL_UNAVAILABLE
} ClassicRetirementKind;

static struct {
    char name[23];
    unsigned age;
    ClassicRetirementKind kind;
} classicRetirementRows[32];
static unsigned classicRetirementCount;

static struct {
    PlayerFile player;
    unsigned age;
    unsigned identity;
} outgoingCallups[20];
static uint16_t outgoingNation;

void ClassicCareer_SnapshotCallups(void)
{
    extern uint16_t _careerUnavailablePlayerLookupTable[];
    outgoingNation = _isNationalSelector ? _selTeamNumber : UINT16_MAX;
    memset(outgoingCallups, 0, sizeof outgoingCallups);
    if (outgoingNation == UINT16_MAX)
        return;
    for (unsigned i = 0; i < 20; ++i) {
        unsigned club = _careerUnavailablePlayerLookupTable[i * 2];
        unsigned slot = _careerUnavailablePlayerLookupTable[i * 2 + 1];
        const PlayerFile *player = (const PlayerFile *)
            ((const uint8_t *)&_careerTeam + sizeof(TeamFile) + i * sizeof(PlayerFile));
        if ((club >> 8) >= 80 || slot >= 32 || !player->playerName[0] ||
            (uint8_t)player->playerName[0] == UINT8_MAX)
            continue;
        if (SWOS_IdentityAgeForClub(club, slot, &outgoingCallups[i].age,
                                   &outgoingCallups[i].identity))
            outgoingCallups[i].player = *player;
    }
}

static void recordClassicRetirement(const PlayerFile *player, unsigned age,
                                    ClassicRetirementKind kind)
{
    if (classicRetirementCount >= sizeof classicRetirementRows / sizeof classicRetirementRows[0])
        return;
    unsigned i = classicRetirementCount++;
    snprintf(classicRetirementRows[i].name, sizeof classicRetirementRows[i].name,
             "%.22s", player->playerName);
    classicRetirementRows[i].age = age;
    classicRetirementRows[i].kind = kind;
}

void SWOS_RetirementNewsRecord(const PlayerFile *player, unsigned age)
{
    recordClassicRetirement(player, age, CLUB_RETIREMENT);
}

void SWOS_NationalSquadUnavailableNewsRecord(const PlayerFile *player)
{
    recordClassicRetirement(player, 0, NATIONAL_UNAVAILABLE);
}

static void showClassicRetirementNews(ClassicRetirementKind kind, const char *title)
{
    static const char *const next[] = { "NEXT" };
    static const char *const done[] = { "CONTINUE" };
    unsigned rows[32];
    unsigned count = 0;
    for (unsigned i = 0; i < classicRetirementCount; ++i)
        if (classicRetirementRows[i].kind == kind)
            rows[count++] = i;
    const unsigned pageSize = 6;
    unsigned pages = (count + pageSize - 1) / pageSize;
    for (unsigned i = 0; i < count;) {
        char text[256];
        unsigned page = i / pageSize;
        size_t used = (size_t)snprintf(text, sizeof text, "PAGE %u OF %u||", page + 1, pages);
        unsigned end = (page + 1) * pageSize;
        for (; i < end && i < count; ++i) {
            unsigned row = rows[i];
            if (kind == NATIONAL_UNAVAILABLE)
                used += (size_t)snprintf(text + used, sizeof text - used,
                                        "%s|", classicRetirementRows[row].name);
            else
                used += (size_t)snprintf(text + used, sizeof text - used, "%s  (AGE %u)|",
                                        classicRetirementRows[row].name, classicRetirementRows[row].age);
        }
        if (MenuDialog_Show(title, text, i < count ? next : done, 1, 0) < 0)
            break;
    }
}

void ClassicCareer_RunTransfers(void)
{
    SWOS_AI_RealTransferTest();
}

void ClassicCareer_AgePlayers(void)
{
    SWOS_AgesAdvanceOneSeason();
    SWOS_IdentityAdvanceSeason();
}

void ClassicCareer_AdvancePlayers(void)
{
    classicRetirementCount = 0;
    SWOS_IdentityPrepareSave();
    SWOS_ManualIdentityPrepareSave();
    SWOS_RetirementDryRun(_currentSeasonShowing);
    SWOS_RetirementLiveAI(_currentSeasonShowing);
    ClassicCareer_RunTransfers();

    if (_isNationalSelector && outgoingNation == _selTeamNumber) {
        extern void Career_RemoveInternationalPlayersByMask(uint32_t mask);
        uint32_t removed = 0;
        for (unsigned i = 0; i < 20; ++i) {
            if (outgoingCallups[i].identity && SWOS_RetiredIdentityContains(outgoingCallups[i].identity)) {
                removed |= UINT32_C(1) << i;
                recordClassicRetirement(&outgoingCallups[i].player, outgoingCallups[i].age,
                                        NATIONAL_RETIREMENT);
            }
        }
        if (removed)
            Career_RemoveInternationalPlayersByMask(removed);
    }
    outgoingNation = UINT16_MAX;
    /* Season setup can load national teams before the club rosters roll over. */
    extern void ClassicNational_Reset(void);
    ClassicNational_Reset();
    extern void SWOS_NationalReconcileAfterSeason(void);
    SWOS_NationalReconcileAfterSeason();
    if (!SWOS_CareerIntegrityCheck("season_end", _currentSeasonShowing))
        swos_age_log("CAREER_INTEGRITY_UNRESOLVED",
                     "season=%u save_preflight_will_recheck=1", (unsigned)_careerSeasonsCompleted);
    SWOS_WorldQualitySnapshot(_currentSeasonShowing);

    extern int CareerAutopilot_Active(void);
    if (!CareerAutopilot_Active()) {
        showClassicRetirementNews(CLUB_RETIREMENT, "PLAYER RETIREMENTS");
        showClassicRetirementNews(NATIONAL_RETIREMENT, "NATIONAL TEAM RETIREMENTS");
        showClassicRetirementNews(NATIONAL_UNAVAILABLE, "NATIONAL TEAM SQUAD CHANGES");
    }
}
#endif
