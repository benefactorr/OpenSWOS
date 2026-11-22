/* swcc: the spec compiler and validator. Spec JSON (schema swc/1) + a host profile -> diagnostics, and when there
   is no error, the binary spec (.SWC, big-endian, docs/format.md).

   Library: swcc_compile() (ctypes from Python, later WASM in the patcher page). CLI: swcc_main.c.
   Every diagnostic is "file:line:col: error|warning: <json path>: message". */
#include "swcc.h"
#include "json.h"
#include "../format/swc_format.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(SwcHeader) == 32, "header");
_Static_assert(sizeof(SwcSection) == 12, "section");
_Static_assert(sizeof(SwcName) == 36, "name");
_Static_assert(sizeof(SwcClub) == 16, "club");
_Static_assert(sizeof(SwcComp) == 28, "comp");
_Static_assert(sizeof(SwcStage) == 24, "stage");
_Static_assert(sizeof(SwcRound) == 8, "round");
_Static_assert(sizeof(SwcOutcome) == 10, "outcome");
_Static_assert(sizeof(SwcReg) == 4, "reg");
_Static_assert(sizeof(SwcMeta) == 8, "meta");
_Static_assert(sizeof(SwcFixed) == 6, "fixed");

/* ---- growable buffers -------------------------------------------------------------------------------------------- */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void *bput(Buf *b, const void *src, size_t n)
{
    void *at;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n) cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) { fprintf(stderr, "swcc: out of memory\n"); exit(2); }
        b->cap = cap;
    }
    at = b->p + b->n;
    if (src) memcpy(at, src, n); else memset(at, 0, n);
    b->n += n;
    return at;
}

/* ---- compiler state ---------------------------------------------------------------------------------------------- */
typedef struct {
    const char *file;
    Buf diag;
    int errors, warnings;
    /* profile */
    char langs[SWC_LANGS][4];
    int nlangs;
    unsigned char charset[256];
    int latin1;                 /* encoding: 0 = ascii, 1 = latin1 (+ overrides) */
    uint32_t over_cp[32];       /* codepoints above 0xFF mapped to a host byte (OpenSWOS: Œ 0x8C, Ÿ 0x9E, € 0x9D) */
    uint8_t over_b[32];
    int nover;
    int width[256];
    int title_max, title_px, short_px, club_name_max, clubs_per_file, table_rows, long_table_rows, max_teams;
    uint32_t arena_max;
    /* spec */
    JNode *root;
    JNode *countries;
    JNode **clubs, **comps;
    int nclubs, ncomps;
    void *stinfo;               /* StageInfo[], all stages in order */
    int nstinfo;
    /* output */
    Buf strs, names, club, comp, stag, entr, tieb, deci, roun, outc, reg, meta, fixt, grps, srce;
    uint32_t arena;
} C;

static void diag(C *c, JNode *at, int error, const char *path, const char *fmt, ...)
{
    char msg[512], line[1024];
    va_list ap;
    int k;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    k = snprintf(line, sizeof line, "%s:%d:%d: %s: %s: %s\n", c->file, at ? at->line : 1, at ? at->col : 1,
                 error ? "error" : "warning", path && *path ? path : "(root)", msg);
    bput(&c->diag, line, (size_t)(k < (int)sizeof line ? k : (int)sizeof line - 1));
    if (error) c->errors++; else c->warnings++;
}
#define ERR(at, path, ...)  diag(c, at, 1, path, __VA_ARGS__)
#define WARN(at, path, ...) diag(c, at, 0, path, __VA_ARGS__)

static const char *pathf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return buf;
}

/* ---- typed member access ----------------------------------------------------------------------------------------- */
static JNode *need(C *c, JNode *obj, const char *key, JType t, const char *path)
{
    JNode *v = json_get(obj, key);
    if (!v) { ERR(obj, path, "missing '%s'", key); return NULL; }
    if (v->type != t) { ERR(v, path, "'%s' must be a %s, not a %s", key, json_type_name(t), json_type_name(v->type)); return NULL; }
    return v;
}

static JNode *opt(C *c, JNode *obj, const char *key, JType t, const char *path)
{
    JNode *v = json_get(obj, key);
    if (v && v->type != t) { ERR(v, path, "'%s' must be a %s, not a %s", key, json_type_name(t), json_type_name(v->type)); return NULL; }
    return v;
}

static int int_in(C *c, JNode *v, const char *key, const char *path, long lo, long hi, long *out)
{
    if (!v) return 0;
    if (v->type != J_NUM || !v->is_int || v->num < lo || v->num > hi) {
        ERR(v, path, "'%s' must be an integer in %ld..%ld", key, lo, hi);
        return 0;
    }
    *out = (long)v->num;
    return 1;
}

static void unknown_keys(C *c, JNode *obj, const char *path)
{
    JNode *m;
    if (!obj || obj->type != J_OBJ) return;
    for (m = obj->child; m; m = m->next)
        if (!m->used && m->key[0] != '_')          /* '_note' style members are comments */
            ERR(m, path, "unknown member '%s'", m->key);
}

static int enum_of(const char *s, const char *const *names, int n)
{
    int i;
    for (i = 0; i < n; i++) if (names[i] && !strcmp(s, names[i])) return i;
    return -1;
}

static void list_names(char *buf, size_t n, const char *const *names, int k)
{
    int i;
    size_t at = 0;
    buf[0] = 0;
    for (i = 0; i < k; i++)
        if (names[i] && at < n) at += (size_t)snprintf(buf + at, n - at, "%s%s", at ? ", " : "", names[i]);
}

/* ---- ids and strings --------------------------------------------------------------------------------------------- */
static int id_ok(const char *s, const char *prefix)
{
    size_t k = prefix ? strlen(prefix) : 0;
    int prev = '.';
    if (prefix && strncmp(s, prefix, k)) return 0;
    s += k;
    if (!*s) return 0;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) prev = ch;
        else if ((ch == '.' || ch == '-' || ch == '_') && prev != '.' && prev != '-' && prev != '_') prev = ch;
        else return 0;
    }
    return prev != '.' && prev != '-' && prev != '_';
}

static uint32_t str_add(C *c, const char *s)
{
    size_t i = 0, k = strlen(s);
    while (i < c->strs.n) {                                    /* dedupe (linear: specs are small) */
        size_t l = strlen((char *)c->strs.p + i);
        if (l == k && !memcmp(c->strs.p + i, s, k)) return (uint32_t)i;
        i += l + 1;
    }
    i = c->strs.n;
    bput(&c->strs, s, k + 1);
    return (uint32_t)i;
}

/* UTF-8 -> the host's bytes (profile "encoding": ascii, or latin1 + "overrides"). Returns 0 and the first codepoint
   that has no host byte in *bad; out holds at most 255 bytes + NUL. */
static int host_conv(C *c, const char *s, char *out, size_t room, uint32_t *bad)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t k = 0;
    while (*p) {
        uint32_t cp = *p;
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0, i, j, b = -1;
        if (extra) {
            cp &= 0x3Fu >> extra;
            for (i = 1; i <= extra; i++) { if ((p[i] & 0xC0) != 0x80) { *bad = 0xFFFD; return 0; } cp = cp << 6 | (p[i] & 63); }
        }
        p += 1 + extra;
        for (j = 0; j < c->nover; j++) if (c->over_cp[j] == cp) b = c->over_b[j];
        if (b < 0 && (cp < 0x80 || (c->latin1 && cp <= 0xFF))) b = (int)cp;
        if (b < 0 || k + 1 >= room) { *bad = cp; return 0; }
        out[k++] = (char)b;
    }
    out[k] = 0;
    return 1;
}

/* A display text: converted to the host's bytes, every byte in the host font. Returns the converted text (valid until
   the next call) or NULL after reporting the error. */
static const char *text_ok(C *c, JNode *at, const char *path, const char *what, const char *s)
{
    static char out[256];
    uint32_t cp = 0;
    const unsigned char *q;
    if (!host_conv(c, s, out, sizeof out, &cp)) {
        if (cp >= 0x20 && cp < 0x7F) ERR(at, path, "%s \"%s\": character '%c' is not in the host font", what, s, (int)cp);
        else ERR(at, path, "%s \"%s\": character U+%04X has no byte in the host's encoding (%s)", what, s, cp,
                 c->latin1 ? "latin1 + overrides" : "ascii: write it without accents, upper case");
        return NULL;
    }
    for (q = (const unsigned char *)out; *q; q++)
        if (!c->charset[*q]) {
            uint32_t u = *q;
            if (u >= 0x20 && u < 0x7F)
                ERR(at, path, "%s \"%s\": character '%c' is not in the host font (allowed: the profile's charset)", what, s, (int)u);
            else
                ERR(at, path, "%s \"%s\": character U+%04X is not in the host font (write it without accents, upper case)", what, s, u);
            return NULL;
        }
    return out;
}

static int text_px(C *c, const char *s)
{
    int w = 0;
    for (; *s; s++) w += c->width[(unsigned char)*s];
    return w;
}

/* ---- profile ----------------------------------------------------------------------------------------------------- */
static int load_profile(C *c, JNode *p)
{
    JNode *v, *m;
    long x;
    int i;
    const char *path = "profile";
    if (!p || p->type != J_OBJ) { ERR(p, path, "the profile must be an object"); return 0; }
    json_get(p, "id");
    if ((v = need(c, p, "languages", J_ARR, path))) {
        for (m = v->child; m; m = m->next) {
            if (m->type != J_STR || strlen(m->str) > 3 || c->nlangs == SWC_LANGS) { ERR(m, path, "bad language"); continue; }
            strcpy(c->langs[c->nlangs++], m->str);
        }
    }
    if ((v = opt(c, p, "encoding", J_STR, path))) {
        if (!strcmp(v->str, "latin1")) c->latin1 = 1;
        else if (strcmp(v->str, "ascii")) ERR(v, path, "encoding \"%s\": ascii or latin1", v->str);
    }
    if ((v = opt(c, p, "overrides", J_OBJ, path)))
        for (m = v->child; m; m = m->next) {
            const unsigned char *k = (const unsigned char *)m->key;     /* the key's one codepoint */
            uint32_t cp = *k;
            int ex = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0, t;
            if (c->nover == 32 || !int_in(c, m, m->key, path, 0x80, 0xFF, &x)) continue;
            cp &= 0x3Fu >> ex;
            for (t = 1; t <= ex; t++) cp = cp << 6 | (k[t] & 63);
            c->over_cp[c->nover] = cp;
            c->over_b[c->nover++] = (uint8_t)x;
        }
    if ((v = need(c, p, "charset", J_STR, path))) {
        char hb[512];
        uint32_t bad = 0;
        if (!host_conv(c, v->str, hb, sizeof hb, &bad)) ERR(v, path, "charset: U+%04X has no byte in the profile's encoding", bad);
        else for (i = 0; hb[i]; i++) c->charset[(unsigned char)hb[i]] = 1;
    }
    if ((v = need(c, p, "widths", J_OBJ, path))) {
        long def = 6;
        int_in(c, json_get(v, "default"), "default", path, 0, 64, &def);
        for (i = 0; i < 256; i++) c->width[i] = (int)def;
        for (m = v->child; m; m = m->next)
            if (strcmp(m->key, "default")) {
                char hb[8];
                uint32_t bad = 0;
                if (!host_conv(c, m->key, hb, sizeof hb, &bad) || strlen(hb) != 1 || !int_in(c, m, m->key, path, 0, 64, &x)) {
                    ERR(m, path, "bad width entry"); continue; }
                c->width[(unsigned char)hb[0]] = (int)x;
            }
    }
#define PI(key, field, lo, hi) x = 0; if (int_in(c, need(c, p, key, J_NUM, path), key, path, lo, hi, &x)) c->field = (int)x;
    PI("title_max", title_max, 1, 255)
    PI("title_px", title_px, 1, 4096)
    PI("short_px", short_px, 1, 4096)
    PI("club_name_max", club_name_max, 1, 255)
    PI("clubs_per_file", clubs_per_file, 1, 65535)
    PI("table_rows", table_rows, 1, 255)
    PI("long_table_rows", long_table_rows, 1, 255)
    PI("max_teams_per_stage", max_teams, 2, 65535)
    x = 0;
    if (int_in(c, need(c, p, "arena_max", J_NUM, path), "arena_max", path, 1024, 0x7FFFFFFF, &x)) c->arena_max = (uint32_t)x;
#undef PI
    unknown_keys(c, p, path);
    return c->errors == 0;
}

/* ---- names: {"it": ["LONG", "SHORT"], ...} ----------------------------------------------------------------------- */
static uint16_t names_rec(C *c, JNode *names, const char *country_code, const char *path, uint32_t id)
{
    SwcName rec;
    int i;
    char p2[256];
    memset(&rec, 0, sizeof rec);
    rec.id = id;
    for (i = 0; i < c->nlangs; i++) {
        JNode *v = names ? json_get(names, c->langs[i]) : NULL, *lg, *sh;
        const char *country = "";
        pathf(p2, sizeof p2, "%s.names.%s", path, c->langs[i]);
        if (!v) { ERR(names, path, "names: missing language '%s' (every name exists in %d languages)", c->langs[i], c->nlangs); continue; }
        if (v->type != J_ARR || v->n != 2 || (lg = json_at(v, 0))->type != J_STR || (sh = json_at(v, 1))->type != J_STR) {
            ERR(v, p2, "a name is [\"LONG NAME\", \"SHORT\"]");
            continue;
        }
        char hc[256] = "", hl[256], hs[256];
        const char *t;
        uint32_t bad;
        if (country_code && c->countries) {
            JNode *cnames = json_get(json_get(c->countries, country_code), "names");
            JNode *cl = cnames ? json_get(cnames, c->langs[i]) : NULL;
            if (cl && cl->type == J_STR && host_conv(c, cl->str, hc, sizeof hc, &bad)) country = hc;
        }
        hl[0] = hs[0] = 0;
        if ((t = text_ok(c, lg, p2, "long name", lg->str))) strcpy(hl, t);
        if (t && (t = text_ok(c, sh, p2, "short name", sh->str))) strcpy(hs, t);
        if (t) {
            size_t tl = strlen(country) + (*country ? 1 : 0) + strlen(hl);
            char title[600];
            snprintf(title, sizeof title, "%s%s%s", country, *country ? " " : "", hl);
            if ((int)tl > c->title_max)
                ERR(lg, p2, "title \"%s\" is %zu characters, the host's title field holds %d", title, tl, c->title_max);
            if (text_px(c, title) > c->title_px)
                ERR(lg, p2, "title \"%s\" is %d px wide, the host's buttons show %d", title, text_px(c, title), c->title_px);
            if (text_px(c, hs) > c->short_px)
                ERR(sh, p2, "short name \"%s\" is %d px wide, the calendar label holds %d", sh->str, text_px(c, hs), c->short_px);
        }
        rec.str[i][0] = str_add(c, hl);
        rec.str[i][1] = str_add(c, hs);
    }
    unknown_keys(c, names, path);
    bput(&c->names, &rec, sizeof rec);
    return (uint16_t)(c->names.n / sizeof rec - 1);
}

/* ---- clubs ------------------------------------------------------------------------------------------------------- */
static int find_club(C *c, const char *id)
{
    int i;
    for (i = 0; i < c->nclubs; i++) {
        JNode *v = json_get(c->clubs[i], "id");
        if (v && v->type == J_STR && !strcmp(v->str, id)) return i;
    }
    return -1;
}

static int find_comp(C *c, const char *id)
{
    int i;
    for (i = 0; i < c->ncomps; i++) {
        JNode *v = json_get(c->comps[i], "id");
        if (v && v->type == J_STR && !strcmp(v->str, id)) return i;
    }
    return -1;
}

static void do_clubs(C *c, JNode *arr)
{
    int i, j;
    char path[128];
    uint16_t *per_file = calloc(65536, sizeof *per_file);
    for (i = 0; i < c->nclubs; i++) {
        JNode *cl = c->clubs[i], *id, *name, *country, *host, *area;
        SwcClub rec;
        long file = 0, ord = 0;
        memset(&rec, 0xFF, sizeof rec);
        pathf(path, sizeof path, "clubs[%d]", i);
        if (cl->type != J_OBJ) { ERR(cl, path, "a club is an object"); continue; }
        id = need(c, cl, "id", J_STR, path);
        name = need(c, cl, "name", J_STR, path);
        country = need(c, cl, "country", J_STR, path);
        host = need(c, cl, "host", J_OBJ, path);
        area = opt(c, cl, "area", J_STR, path);
        if (id) {
            if (!id_ok(id->str, "club:")) ERR(id, path, "club id \"%s\" must be 'club:' + lower-case words joined by '.', '-' or '_'", id->str);
            for (j = 0; j < i; j++) {
                JNode *o = json_get(c->clubs[j], "id");
                if (o && o->type == J_STR && !strcmp(o->str, id->str)) { ERR(id, path, "club id \"%s\" already used by clubs[%d]", id->str, j); break; }
            }
            rec.id = str_add(c, id->str);
        }
        if (name) {
            const char *hn = text_ok(c, name, path, "club name", name->str);
            if (hn && (int)strlen(hn) > c->club_name_max)
                ERR(name, path, "club name \"%s\" is %zu characters, the host holds %d", name->str, strlen(hn), c->club_name_max);
            rec.name = str_add(c, hn ? hn : "");
        }
        if (country) {
            if (!c->countries || !json_get(c->countries, country->str)) ERR(country, path, "country \"%s\" is not in 'countries'", country->str);
            rec.country = (uint16_t)str_add(c, country->str);
        }
        if (host && int_in(c, need(c, host, "file", J_NUM, path), "file", path, 0, 65535, &file) &&
            int_in(c, need(c, host, "ordinal", J_NUM, path), "ordinal", path, 0, 65535, &ord)) {
            rec.host_file = (uint16_t)file;
            rec.host_ordinal = (uint16_t)ord;
            if (ord >= c->clubs_per_file)
                ERR(host, path, "ordinal %ld: a host file holds %d clubs (0..%d)", ord, c->clubs_per_file, c->clubs_per_file - 1);
            if (++per_file[file] == c->clubs_per_file + 1)
                ERR(host, path, "host file %ld has more than %d clubs", file, c->clubs_per_file);
            for (j = 0; j < i; j++) {
                JNode *oh = json_get(c->clubs[j], "host");
                JNode *of = json_get(oh, "file"), *oo = json_get(oh, "ordinal");
                if (of && oo && of->num == file && oo->num == ord) { ERR(host, path, "host file %ld ordinal %ld already used by clubs[%d]", file, ord, j); break; }
            }
            unknown_keys(c, host, path);
        }
        if (area) rec.area = (uint16_t)str_add(c, area->str);
        unknown_keys(c, cl, path);
        bput(&c->club, &rec, sizeof rec);
    }
    free(per_file);
    (void)arr;
}

/* ---- positions: "3", "3-6"; references: "stage#3-6", "comp.id:stage#1" ------------------------------------------ */
/* Positions "N", "N-M" (1-based) or counted from the bottom "LN" (L1 = the last): encoded 1..127, or 128 + N for
   "LN" (the binary keeps the encoding: docs/format.md). */
static int pos_one(const char *s, char **e, int *v)
{
    int last = *s == 'L';
    long x = strtol(s + last, e, 10);
    if (*e == s + last || x < 1 || x > 127) return 0;
    *v = (int)x + (last ? 128 : 0);
    return 1;
}

static int positions(const char *s, int *a, int *b)
{
    char *e;
    if (!pos_one(s, &e, a)) return 0;
    *b = *a;
    if (*e == '-' && !pos_one(e + 1, &e, b)) return 0;
    return !*e;
}

static int pos_res(int enc, int size) { return enc > 128 ? size - (enc - 128) + 1 : enc; }

/* ---- competitions ------------------------------------------------------------------------------------------------ */
static const char *const KINDS[] = { NULL, "league", "cup", "league-cup", "supercup", "continental", "playoff", "tournament" };
static const char *const STYPES[] = { NULL, "round_robin", "groups", "league_phase", "knockout", "playoff", "split",
                                      "aggregate_table", "average_table", "single_match" };
static const char *const TIEBS[] = { NULL, "points", "goal_difference", "goals_for", "goals_against", "wins", "away_wins",
                                     "away_goals_for", "head_to_head_points", "head_to_head_goal_difference",
                                     "head_to_head_goals_for", "head_to_head_away_goals", "fair_play", "coefficient",
                                     "previous_stage_rank", "play_off_match", "draw_lots", "name", "faithful_1996" };
static const char *const DECIS[] = { NULL, "aggregate", "away_goals", "extra_time", "penalties", "better_placed", "replay",
                                     "draw_lots", "golden_goal" };
static const char *const TAGS[] = { NULL, "champion", "promoted", "relegated", "playoff", "playout", "continental", "qualified" };
static const char *const ZONES[] = { NULL, "CL", "EL", "CONF", "PROM", "P.OFF", "P.OUT", "RETR" };
static const char *const HOMES[] = { "draw", "better_placed", "better_placed_second_leg", "neutral", "fixed" };
static const char *const PAIRS[] = { "draw", "seeded", "bracket", "fixed" };
static const char *const MODES[] = { "career", "season", "preset", "friendly" };

#define NE(a) ((int)(sizeof(a) / sizeof(a[0])))

static int enum_member(C *c, JNode *v, const char *key, const char *path, const char *const *names, int n)
{
    int k;
    char all[512];
    if (!v) return -1;
    if (v->type != J_STR) { ERR(v, path, "'%s' must be a string", key); return -1; }
    k = enum_of(v->str, names, n);
    if (k < 0) { list_names(all, sizeof all, names, n); ERR(v, path, "%s \"%s\" is not one of: %s", key, v->str, all); }
    return k;
}

static uint16_t code_list(C *c, Buf *b, JNode *arr, const char *key, const char *path, const char *const *names, int n,
                          int *count)
{
    uint16_t at = (uint16_t)b->n;
    uint8_t zero = 0;
    JNode *m;
    int k = 0;
    for (m = arr ? arr->child : NULL; m; m = m->next) {
        int e = enum_member(c, m, key, path, names, n);
        if (e > 0) { uint8_t u = (uint8_t)e; bput(b, &u, 1); k++; }
    }
    bput(b, &zero, 1);
    if (count) *count = k;
    return at;
}

/* stage teams, for range checks of outcomes / entrants (0 = unknown yet) */
typedef struct { const char *id; int teams, first, n, type, groups, gmin, gmax, per_group, last_ties; } StageInfo;

static int stage_index(StageInfo *si, int n, const char *id)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(si[i].id, id)) return i;
    return -1;
}

static void do_comp(C *c, int ci, StageInfo *all_st, int *nall)
{
    JNode *cp = c->comps[ci], *id, *names, *kind, *country, *parts, *points, *stages, *outcomes, *modes, *level, *v;
    char path[256], p2[300];
    SwcComp rec;
    int k, teams = 0, first_stage = *nall;
    long x;
    memset(&rec, 0, sizeof rec);
    rec.entr = (uint16_t)(c->entr.n / 2);
    pathf(path, sizeof path, "competitions[%d]", ci);
    if (cp->type != J_OBJ) { ERR(cp, path, "a competition is an object"); return; }
    id = need(c, cp, "id", J_STR, path);
    if (id) {
        pathf(path, sizeof path, "%s", id->str);
        if (!id_ok(id->str, NULL)) ERR(id, path, "competition id \"%s\": lower-case words joined by '.', '-' or '_'", id->str);
        if (find_comp(c, id->str) != ci) ERR(id, path, "competition id \"%s\" used twice", id->str);
        rec.id = str_add(c, id->str);
    }
    kind = need(c, cp, "kind", J_STR, path);
    if ((k = enum_member(c, kind, "kind", path, KINDS, NE(KINDS))) > 0) rec.kind = (uint8_t)k;
    country = need(c, cp, "country", J_STR, path);
    if (country) {
        if (!c->countries || !json_get(c->countries, country->str)) ERR(country, path, "country \"%s\" is not in 'countries'", country->str);
        rec.country = str_add(c, country->str);
    }
    if ((level = opt(c, cp, "level", J_NUM, path)) && int_in(c, level, "level", path, 1, 32, &x)) rec.level = (uint8_t)x;
    if ((v = opt(c, cp, "refill", J_STR, path))) {
        if (strcmp(v->str, "pool")) ERR(v, path, "refill must be \"pool\" (clubs that leave the world are replaced from an off-world pool)");
        rec.flags |= 1;
    }
    if (rec.kind == SWC_K_LEAGUE && !level) ERR(cp, path, "a league needs its pyramid 'level'");
    names = need(c, cp, "names", J_OBJ, path);
    rec.name = names_rec(c, names, country ? country->str : NULL, path, rec.id);

    if ((parts = need(c, cp, "participants", J_OBJ, path))) {
        JNode *cnt = need(c, parts, "count", J_NUM, path), *list = opt(c, parts, "clubs", J_ARR, path),
              *from = opt(c, parts, "from", J_STR, path);
        pathf(p2, sizeof p2, "%s.participants", path);
        if (int_in(c, cnt, "count", p2, 2, c->max_teams, &x)) teams = (int)x;
        if (!list == !from) ERR(parts, p2, "give either 'clubs' (a fixed list) or 'from': \"link\"");
        if (from && strcmp(from->str, "link")) ERR(from, p2, "'from' must be \"link\" (entrants come from other competitions' outcomes)");
        {
            JNode *src = opt(c, parts, "source", J_ARR, p2), *q;
            if (src && !from) ERR(src, p2, "'source' goes with \"from\": \"link\"");
            for (q = src ? src->child : NULL; q; q = q->next) {
                int k2 = q->type == J_STR ? find_comp(c, q->str) : -1;
                if (k2 < 0) ERR(q, p2, "source: no competition \"%s\"", q->type == J_STR ? q->str : "?");
                else { uint16_t pr2[2] = { (uint16_t)ci, (uint16_t)k2 }; bput(&c->srce, pr2, 4); }
            }
        }
        if (list) {
            JNode *m;
            int i = 0;
            if (teams && list->n != teams) ERR(list, p2, "%d clubs listed, 'count' says %d", list->n, teams);
            for (m = list->child; m; m = m->next, i++) {
                int ix, j;
                JNode *o;
                if (m->type != J_STR) { ERR(m, p2, "clubs[%d] must be a club id", i); continue; }
                ix = find_club(c, m->str);
                if (ix < 0) { ERR(m, p2, "club \"%s\" is not in 'clubs'", m->str); continue; }
                for (j = 0, o = list->child; o != m; o = o->next, j++)
                    if (o->type == J_STR && !strcmp(o->str, m->str)) { ERR(m, p2, "club \"%s\" listed twice (clubs[%d])", m->str, j); break; }
                { uint16_t u = (uint16_t)ix; bput(&c->entr, &u, 2); rec.nentr++; }
            }
        }
        unknown_keys(c, parts, p2);
    }
    if ((points = opt(c, cp, "points", J_OBJ, path))) {
        long w = 3, d = 1, l = 0;
        pathf(p2, sizeof p2, "%s.points", path);
        int_in(c, need(c, points, "win", J_NUM, p2), "win", p2, 0, 10, &w);
        int_in(c, need(c, points, "draw", J_NUM, p2), "draw", p2, 0, 10, &d);
        int_in(c, need(c, points, "loss", J_NUM, p2), "loss", p2, 0, 10, &l);
        rec.win = (uint8_t)w; rec.draw = (uint8_t)d; rec.loss = (uint8_t)l;
        unknown_keys(c, points, p2);
    } else { rec.win = 3; rec.draw = 1; }

    rec.first_stage = (uint16_t)(c->stag.n / sizeof(SwcStage));
    if ((stages = need(c, cp, "stages", J_ARR, path))) {
        JNode *st;
        int si = 0;
        if (!stages->n) ERR(stages, path, "a competition has at least one stage");
        for (st = stages->child; st; st = st->next, si++) {
            SwcStage s;
            JNode *sid, *type, *legs, *tb, *rounds, *groups, *entrants;
            int t, ntb = 0;
            char sp[320];
            memset(&s, 0, sizeof s);
            s.comp = (uint16_t)ci;
            s.groups = 1;
            s.teams = (uint16_t)teams;
            pathf(sp, sizeof sp, "%s.stages[%d]", path, si);
            if (st->type != J_OBJ) { ERR(st, sp, "a stage is an object"); continue; }
            sid = need(c, st, "id", J_STR, sp);
            if (sid) {
                char full[300];
                pathf(sp, sizeof sp, "%s.stages.%s", path, sid->str);
                if (!id_ok(sid->str, NULL) || strchr(sid->str, '.')) ERR(sid, sp, "stage id \"%s\": lower-case words joined by '-' or '_'", sid->str);
                if (stage_index(all_st + first_stage, *nall - first_stage, sid->str) >= 0) ERR(sid, sp, "stage id \"%s\" used twice", sid->str);
                pathf(full, sizeof full, "%s:%s", id ? id->str : "?", sid->str);
                s.id = str_add(c, full);
                s.purpose = swc_fnv1a(full, (uint32_t)strlen(full));
            }
            type = need(c, st, "type", J_STR, sp);
            t = enum_member(c, type, "type", sp, STYPES, NE(STYPES));
            if (t > 0) s.type = (uint8_t)t;
            all_st[*nall].groups = 1;
            if ((entrants = opt(c, st, "entrants", J_STR, sp))) {           /* "table#3-6" inside this competition */
                char *hash = strchr(entrants->str, '#');
                int a, b, ix;
                if (!hash || !positions(hash + 1, &a, &b)) ERR(entrants, sp, "entrants \"%s\": write 'stage#N', 'stage#N-M' or 'stage#LN-LM' (from the bottom)", entrants->str);
                else {
                    *hash = 0;
                    ix = stage_index(all_st + first_stage, *nall - first_stage, entrants->str);
                    if (ix < 0) ERR(entrants, sp, "entrants: no earlier stage \"%s\" in this competition", entrants->str);
                    else {
                        StageInfo *src = &all_st[first_stage + ix];
                        int lo = src->gmin ? src->gmin : src->teams, hi = src->gmax ? src->gmax : src->teams;
                        int n1 = pos_res(b, lo) - pos_res(a, lo) + 1, n2 = pos_res(b, hi) - pos_res(a, hi) + 1;
                        JNode *pg = opt(c, st, "per_group", J_BOOL, sp);
                        if (lo && (pos_res(a, lo) < 1 || pos_res(b, lo) > lo || n1 < 1))
                            ERR(entrants, sp, "entrants: positions outside stage \"%s\" (%d clubs per table)", entrants->str, lo);
                        else if (n1 != n2)
                            ERR(entrants, sp, "entrants: the range has %d clubs in a group of %d and %d in a group of %d: count both ends from the same side", n1, lo, n2, hi);
                        if (src->groups > 1 && (pg ? pg->b : t == SWC_ST_PLAYOFF)) {   /* one play-off per group */
                            all_st[*nall].per_group = 1;
                            all_st[*nall].groups = src->groups;
                            s.groups = (uint16_t)src->groups;
                            s.flags |= 1;
                            s.teams = (uint16_t)n1;
                        } else s.teams = (uint16_t)(n1 * src->groups);
                    }
                    *hash = '#';
                }
            }
            legs = opt(c, st, "legs", J_NUM, sp);
            if (legs && int_in(c, legs, "legs", sp, 1, 4, &x)) s.legs = (uint8_t)x;
            groups = opt(c, st, "groups", J_NUM, sp);
            if (groups && int_in(c, groups, "groups", sp, 1, 64, &x)) s.groups = (uint16_t)x;
            if (t == SWC_ST_GROUPS || t == SWC_ST_ROUND_ROBIN) {
                JNode *sz = opt(c, st, "sizes", J_ARR, sp), *q;
                int g = s.groups ? s.groups : 1;
                all_st[*nall].groups = g;
                if (sz) {
                    int sum = 0, mn = 999, mx = 0;
                    if (sz->n != g) ERR(sz, sp, "sizes: %d listed, 'groups' says %d", sz->n, g);
                    for (q = sz->child; q; q = q->next) {
                        long k = 0;
                        if (!int_in(c, q, "sizes[]", sp, 2, 255, &k)) continue;
                        sum += (int)k; mn = k < mn ? (int)k : mn; mx = k > mx ? (int)k : mx;
                        { uint16_t gr[2] = { (uint16_t)(c->stag.n / sizeof(SwcStage)), (uint16_t)k }; bput(&c->grps, gr, 4); }
                    }
                    if (s.teams && sum != s.teams) ERR(sz, sp, "sizes add up to %d, the stage has %d clubs", sum, s.teams);
                    all_st[*nall].gmin = mn; all_st[*nall].gmax = mx;
                } else {
                    if (g > 1 && s.teams % g) ERR(st, sp, "%d clubs do not split into %d equal groups: give 'sizes'", s.teams, g);
                    all_st[*nall].gmin = all_st[*nall].gmax = s.teams / g;
                }
            }
            if (t == SWC_ST_ROUND_ROBIN || t == SWC_ST_GROUPS) {
                int per;
                if (!legs) ERR(st, sp, "a %s stage needs 'legs' (meetings per pair)", STYPES[t]);
                tb = need(c, st, "tiebreakers", J_ARR, sp);
                s.tieb = code_list(c, &c->tieb, tb, "tie-breaker", sp, TIEBS, NE(TIEBS), &ntb);
                if (tb && !ntb) ERR(tb, sp, "tiebreakers: at least one criterion");
                per = all_st[*nall].gmax;
                if (per > c->long_table_rows)
                    ERR(st, sp, "%d clubs in one table: the host shows at most %d", per, c->long_table_rows);
                else if (per > c->table_rows && !(json_get(st, "long_table") && json_get(st, "long_table")->b))
                    ERR(st, sp, "%d clubs in one table: over %d needs \"long_table\": true (scrolling table)", per, c->table_rows);
                if (s.teams && s.teams < 2) ERR(st, sp, "a table needs at least 2 clubs");
            } else if (t == SWC_ST_KNOCKOUT || t == SWC_ST_PLAYOFF) {
                JNode *r;
                int ri = 0, left = s.teams;
                s.first_round = (uint16_t)(c->roun.n / sizeof(SwcRound));
                rounds = need(c, st, "rounds", J_ARR, sp);
                for (r = rounds ? rounds->child : NULL; r; r = r->next, ri++) {
                    SwcRound rr;
                    JNode *dec;
                    char rp[360];
                    int nd = 0, h, pr;
                    long ties = 0, rl = 1, byes = 0;
                    memset(&rr, 0, sizeof rr);
                    pathf(rp, sizeof rp, "%s.rounds[%d]", sp, ri);
                    if (r->type != J_OBJ) { ERR(r, rp, "a round is an object"); continue; }
                    int_in(c, need(c, r, "ties", J_NUM, rp), "ties", rp, 1, 1024, &ties);
                    int_in(c, need(c, r, "legs", J_NUM, rp), "legs", rp, 1, 2, &rl);
                    int_in(c, opt(c, r, "byes", J_NUM, rp), "byes", rp, 0, 1024, &byes);
                    h = enum_member(c, need(c, r, "home", J_STR, rp), "home", rp, HOMES, NE(HOMES));
                    pr = enum_member(c, need(c, r, "pairing", J_STR, rp), "pairing", rp, PAIRS, NE(PAIRS));
                    dec = json_get(r, "decider");
                    if (!dec) ERR(r, rp, "missing 'decider': every round says how a level tie ends (e.g. [\"aggregate\", \"extra_time\", \"penalties\"])");
                    else if (dec->type != J_ARR) ERR(dec, rp, "'decider' must be an array");
                    rr.deci = code_list(c, &c->deci, dec, "decider", rp, DECIS, NE(DECIS), &nd);
                    if (dec && dec->type == J_ARR && !nd) ERR(dec, rp, "decider: at least one entry");
                    if (pr == 3) {                          /* pairing "fixed": the draw as really made */
                        JNode *fx = need(c, r, "fixed", J_ARR, rp), *pair;
                        int fi = 0;
                        if (fx && fx->n != ties) ERR(fx, rp, "fixed: %d pairs listed, 'ties' says %ld", fx->n, ties);
                        for (pair = fx ? fx->child : NULL; pair; pair = pair->next, fi++) {
                            JNode *h1 = json_at(pair, 0), *a1 = json_at(pair, 1);
                            SwcFixed f;
                            int hx, ax;
                            if (pair->type != J_ARR || pair->n != 2 || h1->type != J_STR || a1->type != J_STR) {
                                ERR(pair, rp, "fixed[%d]: a pair is [\"home club id\", \"away club id\"]", fi);
                                continue;
                            }
                            hx = find_club(c, h1->str);
                            ax = find_club(c, a1->str);
                            if (hx < 0) ERR(h1, rp, "fixed[%d]: club \"%s\" is not in 'clubs'", fi, h1->str);
                            if (ax < 0) ERR(a1, rp, "fixed[%d]: club \"%s\" is not in 'clubs'", fi, a1->str);
                            if (hx >= 0 && hx == ax) ERR(a1, rp, "fixed[%d]: a club cannot play itself", fi);
                            if (ri == 0 && hx >= 0 && ax >= 0) {   /* first round: both must be participants, once */
                                const uint16_t *en = (const uint16_t *)c->entr.p + rec.entr;
                                int k2, inh = 0, ina = 0;
                                JNode *q;
                                for (k2 = 0; k2 < rec.nentr; k2++) { inh |= en[k2] == hx; ina |= en[k2] == ax; }
                                if (rec.nentr && (!inh || !ina)) ERR(pair, rp, "fixed[%d]: both clubs must be participants of the competition", fi);
                                for (q = fx->child; q != pair; q = q->next) {
                                    JNode *qh = json_at(q, 0), *qa = json_at(q, 1);
                                    if (q->type == J_ARR && q->n == 2 && qh->type == J_STR && qa->type == J_STR &&
                                        (!strcmp(qh->str, h1->str) || !strcmp(qa->str, h1->str) ||
                                         !strcmp(qh->str, a1->str) || !strcmp(qa->str, a1->str)))
                                        ERR(pair, rp, "fixed[%d]: a club is drawn twice in this round", fi);
                                }
                            }
                            f.round = (uint16_t)(c->roun.n / sizeof(SwcRound));
                            f.home = (uint16_t)(hx < 0 ? 0 : hx);
                            f.away = (uint16_t)(ax < 0 ? 0 : ax);
                            bput(&c->fixt, &f, sizeof f);
                        }
                    } else if (json_get(r, "fixed")) ERR(json_get(r, "fixed"), rp, "'fixed' pairs need \"pairing\": \"fixed\"");
                    if (left && ties * 2 + byes != left)
                        ERR(r, rp, "%ld ties + %ld byes take %ld clubs, %d reach this round", ties, byes, ties * 2 + byes, left);
                    left = (int)(ties + byes);
                    all_st[*nall].last_ties = (int)ties;
                    rr.ties = (uint16_t)ties;
                    rr.legs = (uint8_t)rl;
                    rr.home = (uint8_t)(h < 0 ? 0 : h);
                    rr.pairing = (uint8_t)(pr < 0 ? 0 : pr);
                    unknown_keys(c, r, rp);
                    bput(&c->roun, &rr, sizeof rr);
                    s.nrounds++;
                }
                if (rounds && !rounds->n) ERR(rounds, sp, "rounds: at least one");
                if (rounds && left != 1 && s.teams && t == SWC_ST_KNOCKOUT)    /* (a play-off may qualify several) */
                    ERR(rounds, sp, "the rounds end with %d clubs, not one winner", left);
            }
            json_get(st, "long_table");
            json_get(st, "per_group");
            unknown_keys(c, st, sp);
            bput(&c->stag, &s, sizeof s);
            all_st[*nall].id = sid ? sid->str : "";
            all_st[*nall].teams = s.teams;
            all_st[*nall].type = t;
            (*nall)++;
            rec.nstages++;
            c->arena += 64u + (uint32_t)s.teams * 32u + (uint32_t)s.nrounds * 16u;
        }
    }
    rec.first_outc = (uint16_t)(c->outc.n / sizeof(SwcOutcome));
    if ((outcomes = opt(c, cp, "outcomes", J_ARR, path))) {
        JNode *o;
        int oi = 0;
        for (o = outcomes->child; o; o = o->next, oi++) {
            SwcOutcome r;
            JNode *st, *pos, *tag, *zone, *to, *season;
            int a = 0, b = 0, sx, tg, z;
            char op[320];
            memset(&r, 0, sizeof r);
            r.target = 0xFFFF;
            pathf(op, sizeof op, "%s.outcomes[%d]", path, oi);
            if (o->type != J_OBJ) { ERR(o, op, "an outcome is an object"); continue; }
            st = need(c, o, "stage", J_STR, op);
            pos = need(c, o, "positions", J_STR, op);
            tag = need(c, o, "tag", J_STR, op);
            zone = opt(c, o, "zone", J_STR, op);
            to = opt(c, o, "to", J_STR, op);
            season = opt(c, o, "season", J_STR, op);
            sx = st ? stage_index(all_st + first_stage, *nall - first_stage, st->str) : -1;
            if (st && sx < 0) ERR(st, op, "stage \"%s\" is not a stage of this competition", st->str);
            if (pos && !positions(pos->str, &a, &b)) ERR(pos, op, "positions \"%s\": write 'N', 'N-M' (1-based) or 'LN' (from the bottom)", pos->str);
            else if (pos && sx >= 0) {
                StageInfo *si = &all_st[first_stage + sx];
                int ko = si->type == SWC_ST_KNOCKOUT || si->type == SWC_ST_PLAYOFF;
                int lo = ko ? 2 : (si->gmin ? si->gmin : si->teams), hi = ko ? 2 : (si->gmax ? si->gmax : si->teams);
                if (ko && (a > 2 || b > 2)) ERR(pos, op, "positions \"%s\": a knockout stage ranks 1 (winners) and 2 (losers of the last round)", pos->str);
                else if (lo && (pos_res(a, lo) < 1 || pos_res(b, hi) > hi || pos_res(b, lo) < pos_res(a, lo)))
                    ERR(pos, op, "position %d, stage \"%s\" ranks %d clubs", pos_res(b, hi), st->str, hi);
            }
            tg = enum_member(c, tag, "tag", op, TAGS, NE(TAGS));
            z = zone ? enum_member(c, zone, "zone", op, ZONES, NE(ZONES)) : 0;
            if (to && !strcmp(to->str, "out")) r.target = 0xFFFE;         /* leaves the world */
            else if (to) {
                int t2 = find_comp(c, to->str);
                if (t2 < 0) ERR(to, op, "'to': no competition \"%s\" in this world", to->str);
                else if (t2 == ci && !season) ERR(to, op, "'to' its own competition needs \"season\": \"next\"");
                r.target = (uint16_t)(t2 < 0 ? 0xFFFF : t2);
            }
            if ((tg == SWC_TAG_PROMOTED || tg == SWC_TAG_RELEGATED) && !to) ERR(o, op, "a %s outcome needs 'to'", TAGS[tg]);
            if (season) {
                if (!strcmp(season->str, "next")) r.season = 1;
                else if (strcmp(season->str, "this")) ERR(season, op, "season must be \"this\" or \"next\"");
            }
            r.stage = (uint16_t)(sx < 0 ? 0 : rec.first_stage + sx);
            r.from = (uint8_t)a; r.to = (uint8_t)b;
            r.tag = (uint8_t)(tg < 0 ? 0 : tg);
            r.zone = (uint8_t)(z < 0 ? 0 : z);
            unknown_keys(c, o, op);
            bput(&c->outc, &r, sizeof r);
            rec.noutc++;
        }
    }
    if ((modes = opt(c, cp, "modes", J_ARR, path))) {
        JNode *m;
        SwcReg rg = { (uint16_t)ci, 0 };
        for (m = modes->child; m; m = m->next) {
            int e = enum_member(c, m, "mode", path, MODES, NE(MODES));
            if (e >= 0) rg.modes |= (uint16_t)(1u << e);
        }
        bput(&c->reg, &rg, sizeof rg);
    } else WARN(cp, path, "no 'modes': the competition is offered in no game mode (every world should be playable in all modes)");
    if ((v = json_get(cp, "_note"))) (void)v;
    unknown_keys(c, cp, path);
    bput(&c->comp, &rec, sizeof rec);
}

/* ---- link checks across competitions ----------------------------------------------------------------------------- */
static void check_links(C *c)
{
    /* every league keeps its size: the clubs that leave it (promoted, relegated, out of the world) = the clubs that
       come in; a league with "refill": "pool" is refilled from outside, so only its out-side is counted. A play-off
       whose participants come "from": "link" with a "source" counts its moves for its source leagues. */
    const SwcOutcome *o = (const SwcOutcome *)c->outc.p;
    const SwcComp *cm = (const SwcComp *)c->comp.p;
    const SwcStage *sg = (const SwcStage *)c->stag.p;
    const StageInfo *si = (const StageInfo *)c->stinfo;
    const uint16_t *src = (const uint16_t *)c->srce.p;
    int no = (int)(c->outc.n / sizeof *o), nsrc = (int)(c->srce.n / 4), i, j, k;
    int *out = calloc((size_t)c->ncomps + 1, sizeof *out), *in = calloc((size_t)c->ncomps + 1, sizeof *in);
    for (j = 0; j < no; j++) {
        const StageInfo *x = &si[o[j].stage];
        int ko = x->type == SWC_ST_KNOCKOUT || x->type == SWC_ST_PLAYOFF, n, from = sg[o[j].stage].comp, any = 0;
        if (o[j].tag != SWC_TAG_PROMOTED && o[j].tag != SWC_TAG_RELEGATED) continue;
        if (ko) n = ((o[j].from <= 1 && o[j].to >= 1) + (o[j].from <= 2 && o[j].to >= 2)) * x->last_ties * (x->per_group ? x->groups : 1);
        else n = (pos_res(o[j].to, x->gmin) - pos_res(o[j].from, x->gmin) + 1) * x->groups;
        for (k = 0; k < nsrc; k++)
            if (src[2 * k] == from && cm[src[2 * k + 1]].kind == SWC_K_LEAGUE) { out[src[2 * k + 1]] += n; any = 1; }
        if (!any) out[from] += n;
        if (o[j].target < c->ncomps) in[o[j].target] += n;
    }
    for (i = 0; i < c->ncomps; i++) {
        if (cm[i].kind != SWC_K_LEAGUE || (!in[i] && !out[i])) continue;
        if (cm[i].flags & 1 ? in[i] > out[i] : in[i] != out[i])
            ERR(c->comps[i], swc_str(&(SwcSpec){ .strs = (const char *)c->strs.p, .nstrs = (uint32_t)c->strs.n }, cm[i].id),
                "the league loses %d clubs and gains %d: its size would change%s", out[i], in[i],
                cm[i].flags & 1 ? " (more clubs come in than leave, even with its refill)" : "");
    }
    free(out);
    free(in);
}

/* ---- writer ------------------------------------------------------------------------------------------------------ */
static void pad4(Buf *b) { while (b->n & 3) bput(b, NULL, 1); }

/* Encodes n native records of layout l at src into big-endian bytes at dst (docs/format.md). */
static void encode(uint8_t *dst, const uint8_t *src, size_t n, const char *l)
{
    uint32_t rec = swc_layout_size(l);
    size_t i;
    for (i = 0; i < n; i++) {
        const char *f;
        uint32_t off = 0;
        for (f = l; *f; f++) {
            const uint8_t *q = src + i * rec + off;
            uint8_t *d = dst + i * rec + off;
            if (*f == '1') *d = *q;
            else if (*f == '2') { uint16_t v; memcpy(&v, q, 2); SWC_BE_WRITE_U16(d, v); }
            else { uint32_t v; memcpy(&v, q, 4); SWC_BE_WRITE_U32(d, v); }
            off += (uint32_t)(*f - '0');
        }
    }
}

static uint8_t *write_bin(C *c, int world, size_t *len)
{
    struct { uint32_t tag; Buf *b; } sec[] = {
        { SWC_S_STRS, &c->strs }, { SWC_S_NAME, &c->names }, { SWC_S_CLUB, &c->club }, { SWC_S_COMP, &c->comp },
        { SWC_S_STAG, &c->stag }, { SWC_S_ENTR, &c->entr }, { SWC_S_TIEB, &c->tieb }, { SWC_S_DECI, &c->deci },
        { SWC_S_ROUN, &c->roun }, { SWC_S_OUTC, &c->outc }, { SWC_S_REG, &c->reg }, { SWC_S_META, &c->meta },
        { SWC_S_FIXT, &c->fixt }, { SWC_S_GRPS, &c->grps }, { SWC_S_SRCE, &c->srce } };
    int ns = (int)(sizeof sec / sizeof sec[0]), i;
    while (ns > 13 && !sec[ns - 1].b->n) ns--;           /* sections added after the first release: only when used */
    if (ns > 13 && !sec[13].b->n && sec[14].b->n) ns = 15;
    Buf out = { 0 };
    uint8_t *h;
    size_t body;
    bput(&out, NULL, 32 + (size_t)ns * 12);
    body = out.n;
    for (i = 0; i < ns; i++) {
        const char *l = swc_layout(sec[i].tag);
        uint8_t *e;
        size_t at;
        pad4(&out);
        at = out.n;
        bput(&out, NULL, sec[i].b->n);
        if (sec[i].b->n) {
            if (l) encode(out.p + at, sec[i].b->p, sec[i].b->n / swc_layout_size(l), l);
            else memcpy(out.p + at, sec[i].b->p, sec[i].b->n);
        }
        e = out.p + 32 + (size_t)i * 12;
        SWC_BE_WRITE_U32(e, sec[i].tag);
        SWC_BE_WRITE_U32(e + 4, at);
        SWC_BE_WRITE_U32(e + 8, sec[i].b->n);
    }
    pad4(&out);
    h = out.p;
    SWC_BE_WRITE_U32(h, SWC_MAGIC);
    SWC_BE_WRITE_U16(h + 4, SWC_FORMAT);
    SWC_BE_WRITE_U16(h + 6, SWC_VERSION);
    SWC_BE_WRITE_U32(h + 8, 0);
    SWC_BE_WRITE_U16(h + 12, world);
    SWC_BE_WRITE_U16(h + 14, ns);
    SWC_BE_WRITE_U32(h + 20, c->arena);
    SWC_BE_WRITE_U32(h + 24, out.n);
    SWC_BE_WRITE_U32(h + 28, 0);
    SWC_BE_WRITE_U32(h + 16, swc_fnv1a(out.p + body, (uint32_t)(out.n - body)));
    *len = out.n;
    return out.p;
}

/* ---- entry point ------------------------------------------------------------------------------------------------- */
int swcc_compile(const char *spec, size_t len, const char *name, const char *profile, size_t plen,
                 unsigned char **bin, size_t *blen, char **diag_out)
{
    C cc, *c = &cc;
    char err[256];
    JNode *proot, *v, *m;
    StageInfo *st = NULL;
    int nst = 0, i;
    long world = 0;
    memset(c, 0, sizeof *c);
    c->file = name ? name : "spec";
    if (bin) *bin = NULL;
    if (blen) *blen = 0;
    proot = json_parse(profile, plen, err, sizeof err);
    if (!proot) {
        const char *f = c->file;
        c->file = "profile";
        ERR(NULL, "profile", "%s", err);
        c->file = f;
    } else load_profile(c, proot);
    c->root = json_parse(spec, len, err, sizeof err);
    if (!c->root) {
        int l = 1, col = 1;
        char tmp[300];
        sscanf(err, "%d:%d:", &l, &col);
        snprintf(tmp, sizeof tmp, "%s:%d:%d: error: (syntax): %s\n", c->file, l, col, strchr(strchr(err, ':') + 1, ':') + 2);
        bput(&c->diag, tmp, strlen(tmp));
        c->errors++;
    }
    if (c->root && c->errors == 0) {
        if (c->root->type != J_OBJ) ERR(c->root, "", "the spec is a JSON object");
        else {
            if ((v = need(c, c->root, "schema", J_STR, "")) && strcmp(v->str, "swc/1"))
                ERR(v, "", "schema \"%s\": this compiler reads \"swc/1\"", v->str);
            if ((v = need(c, c->root, "id", J_STR, "")) && !id_ok(v->str, NULL)) ERR(v, "", "world id \"%s\": lower-case words joined by '.', '-' or '_'", v->str);
            int_in(c, opt(c, c->root, "host_world", J_NUM, ""), "host_world", "", 0, 65535, &world);
            {
                SwcMeta me;
                JNode *ver = opt(c, c->root, "version", J_STR, "");
                me.id = str_add(c, v && v->type == J_STR ? v->str : "");
                me.version = str_add(c, ver ? ver->str : "0");
                bput(&c->meta, &me, sizeof me);
            }
            if ((v = opt(c, c->root, "languages", J_ARR, ""))) {
                for (i = 0; i < c->nlangs; i++) {
                    int found = 0;
                    for (m = v->child; m; m = m->next) if (m->type == J_STR && !strcmp(m->str, c->langs[i])) found = 1;
                    if (!found) ERR(v, "languages", "the host needs language '%s'", c->langs[i]);
                }
            }
            c->countries = need(c, c->root, "countries", J_OBJ, "");
            for (m = c->countries ? c->countries->child : NULL; m; m = m->next) {
                char cpth[64];
                JNode *cn;
                pathf(cpth, sizeof cpth, "countries.%s", m->key);
                if (strlen(m->key) != 3 || m->key[0] < 'A' || m->key[0] > 'Z') ERR(m, cpth, "a country code is 3 upper-case letters (FIFA)");
                if (m->type != J_OBJ) { ERR(m, cpth, "a country is an object"); continue; }
                if ((cn = need(c, m, "names", J_OBJ, cpth))) {
                    for (i = 0; i < c->nlangs; i++) {
                        JNode *l = json_get(cn, c->langs[i]);
                        if (!l || l->type != J_STR) ERR(cn, cpth, "names: missing language '%s'", c->langs[i]);
                        else text_ok(c, l, cpth, "country name", l->str);
                    }
                    unknown_keys(c, cn, cpth);
                }
                unknown_keys(c, m, cpth);
            }
            if ((v = need(c, c->root, "clubs", J_ARR, ""))) {
                c->nclubs = v->n;
                c->clubs = calloc((size_t)v->n + 1, sizeof *c->clubs);
                for (i = 0, m = v->child; m; m = m->next) c->clubs[i++] = m;
                do_clubs(c, v);
            }
            if ((v = need(c, c->root, "competitions", J_ARR, ""))) {
                int total = 0;
                JNode *s;
                c->ncomps = v->n;
                c->comps = calloc((size_t)v->n + 1, sizeof *c->comps);
                for (i = 0, m = v->child; m; m = m->next) {
                    c->comps[i++] = m;
                    s = json_get(m, "stages");
                    total += s && s->type == J_ARR ? s->n : 0;
                }
                st = calloc((size_t)total + 1, sizeof *st);
                c->stinfo = st;
                for (i = 0; i < c->ncomps; i++) do_comp(c, i, st, &nst);
                if (c->errors == 0) check_links(c);
            }
            unknown_keys(c, c->root, "");
        }
    }
    c->arena += 4096u + (uint32_t)c->nclubs * 8u;
    if (c->errors == 0 && c->arena_max && c->arena > c->arena_max)
        ERR(c->root, "", "the engine would need %u bytes of arena, the host gives %u", c->arena, c->arena_max);
    if (c->errors == 0 && bin) {
        size_t n;
        *bin = write_bin(c, (int)world, &n);
        if (blen) *blen = n;
    }
    bput(&c->diag, "", 1);
    if (diag_out) *diag_out = (char *)c->diag.p; else free(c->diag.p);
    json_free(c->root);
    json_free(proot);
    free(c->clubs); free(c->comps); free(st);
    free(c->strs.p); free(c->names.p); free(c->club.p); free(c->comp.p); free(c->stag.p); free(c->entr.p);
    free(c->tieb.p); free(c->deci.p); free(c->roun.p); free(c->outc.p); free(c->reg.p); free(c->meta.p); free(c->fixt.p); free(c->grps.p); free(c->srce.p);
    return c->errors;
}

void swcc_free(void *p) { free(p); }
