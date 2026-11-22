/* Format reference tests: PCG32 against the PCG reference vectors, FNV-1a, arena, BE accessors, record layouts,
   the reader on the sample pack (aligned, at an odd address, byte-swapped = refused). */
#include "../format/swc_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static void sample(const char *path);

int main(int argc, char **argv)
{
    /* pcg32-demo (pcg-c-basic): seed 42, sequence 54 */
    static const uint32_t ref[6] = { 0xa15c02b7, 0x7b47f409, 0xba1d3330, 0x83d2f293, 0xbfa4784b, 0xcbed606e };
    SwcRng r, r2;
    SwcArena a;
    static uint8_t mem[1024];
    uint8_t *p, *q;
    int i, hist[7] = { 0 };
    swc_rng_seed(&r, 42u, 54u);
    for (i = 0; i < 6; i++) CHECK(swc_rng_next(&r) == ref[i]);
    for (i = 0; i < 7000; i++) { uint32_t x = swc_rng_below(&r, 7); CHECK(x < 7); hist[x]++; }
    for (i = 0; i < 7; i++) CHECK(hist[i] > 850 && hist[i] < 1150);
    swc_rng_stream(&r, 1997, 1, swc_fnv1a("eng.fa-cup:ko", 13));
    swc_rng_stream(&r2, 1997, 1, swc_fnv1a("eng.fa-cup:ko", 13));
    CHECK(swc_rng_next(&r) == swc_rng_next(&r2));
    swc_rng_stream(&r2, 1997, 2, swc_fnv1a("eng.fa-cup:ko", 13));
    CHECK(swc_rng_next(&r) != swc_rng_next(&r2));
    /* FNV-1a 32 reference values */
    CHECK(swc_fnv1a("", 0) == 0x811c9dc5u);
    CHECK(swc_fnv1a("a", 1) == 0xe40c292cu);
    CHECK(swc_fnv1a("foobar", 6) == 0xbf9cf968u);
    /* arena */
    CHECK(swc_init(&a, mem, sizeof mem) == SWC_OK);
    p = swc_alloc(&a, 3);
    q = swc_alloc(&a, 8);
    CHECK(p && q && q - p == 4 && ((uintptr_t)q & 3) == 0);
    CHECK(swc_alloc(&a, 2000) == NULL);
    swc_reset(&a, 4);
    CHECK(a.used == 4 && a.peak == 12);
    /* spec reader rejections (big-endian header written byte by byte) */
    {
        static uint8_t buf[64];
        SwcSpec s;
        swc_init(&a, mem, sizeof mem);
        CHECK(swc_spec_open(&s, buf, 8, &a) == SWC_E_TRUNC);
        CHECK(swc_spec_open(&s, buf, sizeof buf, &a) == SWC_E_MAGIC);
        buf[0] = 'B'; buf[1] = 'C'; buf[2] = 'W'; buf[3] = 'S';          /* the S0 little-endian magic */
        CHECK(swc_spec_open(&s, buf, sizeof buf, &a) == SWC_E_MAGIC);
        SWC_BE_WRITE_U32(buf, SWC_MAGIC);
        CHECK(buf[0] == 'S' && buf[1] == 'W' && buf[2] == 'C' && buf[3] == 'B');
        SWC_BE_WRITE_U16(buf + 4, 1);                                     /* format 1 (S0, little-endian) */
        CHECK(swc_spec_open(&s, buf, sizeof buf, &a) == SWC_E_FORMAT);
        SWC_BE_WRITE_U16(buf + 4, SWC_FORMAT);
        SWC_BE_WRITE_U16(buf + 6, 0x7FFF);
        CHECK(swc_spec_open(&s, buf, sizeof buf, &a) == SWC_E_ENGINE);
    }
    /* BE accessors: independent of the host's byte order and of alignment */
    {
        static const uint8_t be[7] = { 0x00, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc };
        uint8_t w[6];
        CHECK(SWC_BE_READ_U16(be + 1) == 0x1234 && SWC_BE_READ_U32(be + 1) == 0x12345678u && SWC_BE_READ_U32(be + 3) == 0x56789abcu);
        SWC_BE_WRITE_U32(w + 1, 0xdeadbeefu);
        CHECK(w[1] == 0xde && w[2] == 0xad && w[3] == 0xbe && w[4] == 0xef);
        CHECK(swc_layout_size(swc_layout(SWC_S_COMP)) == sizeof(SwcComp) && swc_layout_size(swc_layout(SWC_S_STAG)) == sizeof(SwcStage) &&
              swc_layout_size(swc_layout(SWC_S_NAME)) == sizeof(SwcName) && swc_layout_size(swc_layout(SWC_S_CLUB)) == sizeof(SwcClub) &&
              swc_layout_size(swc_layout(SWC_S_ROUN)) == sizeof(SwcRound) && swc_layout_size(swc_layout(SWC_S_OUTC)) == sizeof(SwcOutcome) &&
              swc_layout_size(SWC_LAYOUT_HEADER) == sizeof(SwcHeader) && swc_layout_size(SWC_LAYOUT_SECTION) == sizeof(SwcSection));
    }
    CHECK(swc_version() == SWC_VERSION);
    if (argc > 1) sample(argv[1]);
    printf("test_format: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}

static void sample(const char *path)
{
    static uint8_t raw[8192], odd[8193], sw[8192], mem[65536];
    FILE *f = fopen(path, "rb");
    uint32_t len, k;
    SwcArena a;
    SwcSpec s, t;
    if (!f) { printf("FAIL: %s\n", path); fails++; return; }
    len = (uint32_t)fread(raw, 1, sizeof raw, f);
    fclose(f);
    swc_init(&a, mem, sizeof mem);
    CHECK(swc_spec_open(&s, raw, len, &a) == SWC_OK);
    CHECK(s.nclubs == 8 && s.ncomps == 2 && s.nfixt == 4 && s.nmeta == 1 && !strcmp(swc_str(&s, s.meta[0].id), "sample"));
    CHECK(!strcmp(swc_str(&s, s.clubs[7].name), "FENWICK TOWN") && s.clubs[7].host_file == 250 && s.clubs[7].host_ordinal == 7);
    CHECK(!strcmp(swc_str(&s, s.names[s.comps[1].name].str[3][0]), "BEISPIELPOKAL"));
    /* the same bytes at an odd address (a 68000 traps on an odd word access): the same records */
    memcpy(odd + 1, raw, len);
    CHECK(swc_spec_open(&t, odd + 1, len, &a) == SWC_OK && t.nclubs == s.nclubs &&
          !memcmp(t.clubs, s.clubs, s.nclubs * sizeof *s.clubs) && !memcmp(t.comps, s.comps, s.ncomps * sizeof *s.comps) &&
          !memcmp(t.stages, s.stages, s.nstages * sizeof *s.stages) && !memcmp(t.fixt, s.fixt, s.nfixt * sizeof *s.fixt));
    /* forced swap: header + directory words byte-swapped (a little-endian writer) are refused, never misread */
    memcpy(sw, raw, len);
    for (k = 0; k < 32 + 13 * 12; k += 4) { uint8_t x = sw[k]; sw[k] = sw[k + 3]; sw[k + 3] = x; x = sw[k + 1]; sw[k + 1] = sw[k + 2]; sw[k + 2] = x; }
    CHECK(swc_spec_open(&t, sw, len, &a) == SWC_E_MAGIC);
    /* one changed byte in a section: the hash refuses it */
    raw[len - 8] ^= 1;
    CHECK(swc_spec_open(&t, raw, len, &a) == SWC_E_HASH);
}
