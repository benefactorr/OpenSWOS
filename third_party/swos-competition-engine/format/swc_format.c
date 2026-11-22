/* swc_format: arena, PCG32, FNV-1a, the reference reader of the compiled spec (big-endian, docs/format.md).
   Freestanding: no library calls, no 64-bit division, no floats. */
#include "swc_format.h"

static void zero(uint8_t *p, uint32_t n) { while (n--) *p++ = 0; }

uint32_t swc_version(void) { return SWC_VERSION; }

/* ---- arena ------------------------------------------------------------------------------------------------------ */
SwcStatus swc_init(SwcArena *a, void *mem, uint32_t size)
{
    if (!a || !mem || size < 16) return SWC_E_ARG;
    a->base = (uint8_t *)mem;
    a->size = size;
    a->used = a->peak = 0;
    zero(a->base, size);
    return SWC_OK;
}

void *swc_alloc(SwcArena *a, uint32_t n)
{
    uint32_t at = (a->used + 3u) & ~3u;
    if (n > a->size || at > a->size - n) return 0;
    a->used = at + n;
    if (a->used > a->peak) a->peak = a->used;
    zero(a->base + at, n);
    return a->base + at;
}

void swc_reset(SwcArena *a, uint32_t mark) { if (mark <= a->used) a->used = mark; }

/* ---- PCG32 (pcg32_srandom_r / pcg32_random_r of the PCG reference, minimal C version) ---------------------------- */
#define PCG_MULT 6364136223846793005ull

uint32_t swc_rng_next(SwcRng *r)
{
    uint64_t old = r->state;
    uint32_t xorshifted, rot;
    r->state = old * PCG_MULT + r->inc;
    xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

void swc_rng_seed(SwcRng *r, uint64_t initstate, uint64_t initseq)
{
    r->state = 0;
    r->inc = (initseq << 1u) | 1u;
    swc_rng_next(r);
    r->state += initstate;
    swc_rng_next(r);
}

uint32_t swc_rng_below(SwcRng *r, uint32_t n)
{
    uint32_t mask = n - 1, x;
    if (n <= 1) return 0;
    mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4; mask |= mask >> 8; mask |= mask >> 16;
    do x = swc_rng_next(r) & mask; while (x >= n);       /* rejection: exact, no modulo bias, no division */
    return x;
}

void swc_rng_stream(SwcRng *r, uint64_t seed, uint32_t season, uint32_t purpose)
{
    uint8_t b[8];
    uint32_t h;
    b[0] = (uint8_t)season; b[1] = (uint8_t)(season >> 8); b[2] = (uint8_t)(season >> 16); b[3] = (uint8_t)(season >> 24);
    b[4] = (uint8_t)purpose; b[5] = (uint8_t)(purpose >> 8); b[6] = (uint8_t)(purpose >> 16); b[7] = (uint8_t)(purpose >> 24);
    h = swc_fnv1a(b, 8);
    swc_rng_seed(r, seed, (uint64_t)h << 32 | purpose);
}

/* ---- FNV-1a 32 -------------------------------------------------------------------------------------------------- */
uint32_t swc_fnv1a_more(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

uint32_t swc_fnv1a(const void *p, uint32_t n) { return swc_fnv1a_more(2166136261u, p, n); }

/* ---- spec reader ------------------------------------------------------------------------------------------------ */
const char *swc_layout(uint32_t tag)
{
    switch (tag) {
    case SWC_S_NAME: return "444444444";
    case SWC_S_CLUB: return "442222";
    case SWC_S_COMP: return "421142222221111";
    case SWC_S_STAG: return "44211222222";
    case SWC_S_ENTR: return "2";
    case SWC_S_ROUN: return "211211";
    case SWC_S_OUTC: return "21111211";
    case SWC_S_REG:  return "22";
    case SWC_S_META: return "44";
    case SWC_S_FIXT: return "222";
    case SWC_S_GRPS: return "22";
    case SWC_S_SRCE: return "22";
    default:         return 0;
    }
}

uint32_t swc_layout_size(const char *l)
{
    uint32_t n = 0;
    while (*l) n += (uint32_t)(*l++ - '0');
    return n;
}

/* Decodes n big-endian records of layout l from src into native records at dst (aligned, same offsets). */
static void decode(uint8_t *dst, const uint8_t *src, uint32_t n, const char *l)
{
    uint32_t rec = swc_layout_size(l), i;
    for (i = 0; i < n; i++) {
        const char *f = l;
        uint32_t off = 0;
        for (; *f; f++) {
            uint8_t *d = dst + i * rec + off;
            const uint8_t *p = src + i * rec + off;
            if (*f == '1') *d = *p;
            else if (*f == '2') *(uint16_t *)(void *)d = SWC_BE_READ_U16(p);
            else *(uint32_t *)(void *)d = SWC_BE_READ_U32(p);
            off += (uint32_t)(*f - '0');
        }
    }
}

static SwcStatus section(SwcSpec *s, SwcArena *a, uint32_t tag, uint32_t rec, const void **out, uint32_t *n, int required)
{
    const uint8_t *dir = s->bin + 32;
    const char *l = swc_layout(tag);
    uint32_t i;
    if (l && swc_layout_size(l) != rec) return SWC_E_FORMAT;       /* this compiler pads the struct: refuse */
    for (i = 0; i < s->h.nsect; i++) {
        const uint8_t *e = dir + i * 12u;
        uint32_t off = SWC_BE_READ_U32(e + 4), len = SWC_BE_READ_U32(e + 8);
        void *native;
        if (SWC_BE_READ_U32(e) != tag) continue;
        if (off > s->len || len > s->len - off || len % rec) return SWC_E_TRUNC;
        *n = len / rec;
        if (!l) { *out = s->bin + off; return SWC_OK; }             /* bytes: in place */
        native = swc_alloc(a, len ? len : 4);
        if (!native) return SWC_E_NOMEM;
        decode((uint8_t *)native, s->bin + off, *n, l);
        *out = native;
        return SWC_OK;
    }
    *out = 0;
    *n = 0;
    return required ? SWC_E_MISSING : SWC_OK;
}

SwcStatus swc_spec_open(SwcSpec *s, const void *bin, uint32_t len, SwcArena *a)
{
    const uint8_t *b = (const uint8_t *)bin;
    uint32_t body, i;
    SwcStatus st;
    if (!s || !bin || !a) return SWC_E_ARG;
    zero((uint8_t *)s, sizeof *s);
    if (len < 32) return SWC_E_TRUNC;
    s->bin = b;
    s->len = len;
    s->h.magic = SWC_BE_READ_U32(b);
    s->h.format = SWC_BE_READ_U16(b + 4);
    s->h.engine_min = SWC_BE_READ_U16(b + 6);
    s->h.flags = SWC_BE_READ_U32(b + 8);
    s->h.world = SWC_BE_READ_U16(b + 12);
    s->h.nsect = SWC_BE_READ_U16(b + 14);
    s->h.hash = SWC_BE_READ_U32(b + 16);
    s->h.arena = SWC_BE_READ_U32(b + 20);
    s->h.length = SWC_BE_READ_U32(b + 24);
    s->h.reserved = SWC_BE_READ_U32(b + 28);
    if (s->h.magic != SWC_MAGIC) return SWC_E_MAGIC;
    if (s->h.format != SWC_FORMAT) return SWC_E_FORMAT;
    if (s->h.engine_min > SWC_VERSION) return SWC_E_ENGINE;
    if (s->h.length != len) return SWC_E_TRUNC;
    body = 32u + s->h.nsect * 12u;
    if (body > len) return SWC_E_TRUNC;
    if (swc_fnv1a(b + body, len - body) != s->h.hash) return SWC_E_HASH;
#define SEC(tag, T, ptr, cnt, req) \
    if ((st = section(s, a, tag, sizeof(T), (const void **)&s->ptr, &s->cnt, req)) != SWC_OK) return st;
    SEC(SWC_S_STRS, char, strs, nstrs, 1)
    SEC(SWC_S_NAME, SwcName, names, nnames, 1)
    SEC(SWC_S_CLUB, SwcClub, clubs, nclubs, 1)
    SEC(SWC_S_COMP, SwcComp, comps, ncomps, 1)
    SEC(SWC_S_STAG, SwcStage, stages, nstages, 1)
    SEC(SWC_S_ENTR, uint16_t, entr, nentr, 0)
    SEC(SWC_S_TIEB, uint8_t, tieb, ntieb, 0)
    SEC(SWC_S_DECI, uint8_t, deci, ndeci, 0)
    SEC(SWC_S_ROUN, SwcRound, rounds, nrounds, 0)
    SEC(SWC_S_OUTC, SwcOutcome, outc, noutc, 0)
    SEC(SWC_S_REG,  SwcReg, reg, nreg, 0)
    SEC(SWC_S_META, SwcMeta, meta, nmeta, 0)
    SEC(SWC_S_FIXT, SwcFixed, fixt, nfixt, 0)
    SEC(SWC_S_GRPS, SwcGroup, grps, ngrps, 0)
    SEC(SWC_S_SRCE, SwcSource, srce, nsrce, 0)
#undef SEC
    if (!s->nstrs || s->strs[s->nstrs - 1]) return SWC_E_TRUNC;       /* the string table ends with a NUL */
    for (i = 0; i < s->ncomps; i++) {                                  /* cross-references in range */
        const SwcComp *c = &s->comps[i];
        if (c->name >= s->nnames || (uint32_t)c->first_stage + c->nstages > s->nstages ||
            (uint32_t)c->entr + c->nentr > s->nentr || (uint32_t)c->first_outc + c->noutc > s->noutc)
            return SWC_E_TRUNC;
    }
    for (i = 0; i < s->nstages; i++)
        if (s->stages[i].comp >= s->ncomps || s->stages[i].tieb > s->ntieb ||
            (uint32_t)s->stages[i].first_round + s->stages[i].nrounds > s->nrounds)
            return SWC_E_TRUNC;
    for (i = 0; i < s->nentr; i++)
        if (s->entr[i] >= s->nclubs) return SWC_E_TRUNC;
    for (i = 0; i < s->nfixt; i++)
        if (s->fixt[i].round >= s->nrounds || s->fixt[i].home >= s->nclubs || s->fixt[i].away >= s->nclubs)
            return SWC_E_TRUNC;
    for (i = 0; i < s->nreg; i++)
        if (s->reg[i].comp >= s->ncomps) return SWC_E_TRUNC;
    for (i = 0; i < s->noutc; i++)
        if (s->outc[i].stage >= s->nstages || (s->outc[i].target < 0xFFFE && s->outc[i].target >= s->ncomps))
            return SWC_E_TRUNC;
    return SWC_OK;
}

const char *swc_str(const SwcSpec *s, uint32_t off) { return off < s->nstrs ? s->strs + off : ""; }
