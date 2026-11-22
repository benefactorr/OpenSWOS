#ifdef SWOS_SDL_BUILD
#include "swos_age_identity_log.h"
#include "swos_io.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>
typedef struct { char name[48]; uint8_t age; } SquadAgeRecord;
static SquadAgeRecord *s_squadAges;
static size_t s_squadAgeCount;
static int s_squadAgesLoaded;
static int s_squadAgeBulkRestore;
static int SquadAgeCompare(const void *a, const void *b)
{
    return strcmp(((const SquadAgeRecord *)a)->name,
                  ((const SquadAgeRecord *)b)->name);
}
static void SquadAgeLoad(void)
{
    char path[512], line[256];
    FILE *f;
    size_t capacity = 0;
    if (s_squadAgesLoaded) return;
    s_squadAgesLoaded = 1;
    /* Team files are usually in DATA/.  swos_data_path() does not
     * automatically prepend DATA, so try both layouts explicitly. */
    {
        static const char *const candidates[] = {
            "DATA/player_ages_by_name.csv",
            "data/player_ages_by_name.csv",
            "player_ages_by_name.csv"
        };
        size_t i;
        f = NULL;
        for (i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
            const char *resolved = swos_data_path(candidates[i], path,
                                                   (uint32_t)sizeof path);
            f = fopen(resolved, "rb");
            if (f) {
                fprintf(stderr, "[squad-age] loading CSV: %s\n", resolved);
                break;
            }
        }
    }
    if (!f) {
        swos_age_log("CSV_MISSING", "fallback=25");
        fprintf(stderr, "[squad-age] CSV not found (tried DATA/player_ages_by_name.csv "
                        "and player_ages_by_name.csv). All ages default to 25.\n");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *comma = strrchr(line, ',');
        char *start = line;
        char *end;
        long age;
        size_t n;
        if (!comma) continue;
        *comma++ = 0;
        age = strtol(comma, &end, 10);
        if (end == comma || age < 15 || age > 60) continue;
        n = strlen(start);
        if (!n || n >= sizeof s_squadAges[0].name) continue;
        if (s_squadAgeCount == capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            SquadAgeRecord *newData = (SquadAgeRecord *)realloc(
                s_squadAges, next * sizeof *newData);
            if (!newData) break;
            s_squadAges = newData;
            capacity = next;
        }
        memcpy(s_squadAges[s_squadAgeCount].name, start, n + 1);
        s_squadAges[s_squadAgeCount].age = (uint8_t)age;
        ++s_squadAgeCount;
    }
    fclose(f);
    qsort(s_squadAges, s_squadAgeCount, sizeof *s_squadAges,
          SquadAgeCompare);
    fprintf(stderr, "[squad-age] loaded %zu player ages\n", s_squadAgeCount);
    swos_age_log("CSV_LOADED", "entries=%zu", s_squadAgeCount);
}
/* Career age snapshot storage. Original SWOS player records have no identity/age
 * field. The current display keys ages by canonical name; save the snapshot
 * inside each JSON career so later changes to the external CSV cannot alter
 * the ages of an existing career. Duplicate names need a future identity pass. */
void SWOS_AgesResetCareer(void)
{
    free(s_squadAges);
    s_squadAges = NULL;
    s_squadAgeCount = 0;
    s_squadAgesLoaded = 0;
    s_squadAgeBulkRestore = 0;
    swos_age_log("AGE_RESET", "snapshot_cleared=1");
}
void SWOS_AgesBeginRestore(void)
{
    SWOS_AgesResetCareer();
    s_squadAgesLoaded = 1;
    s_squadAgeBulkRestore = 1;
    swos_age_log("RESTORE_BEGIN", "name_indexed=1");
}
void SWOS_AgesEndRestore(void)
{
    s_squadAgeBulkRestore = 0;
    qsort(s_squadAges, s_squadAgeCount, sizeof *s_squadAges, SquadAgeCompare);
    swos_age_log("RESTORE_END", "entries=%zu", s_squadAgeCount);
}
size_t SWOS_AgesCount(void)
{
    SquadAgeLoad();
    return s_squadAgeCount;
}
int SWOS_AgesGet(size_t i, const char **name, unsigned *age)
{
    SquadAgeLoad();
    if (i >= s_squadAgeCount) return 0;
    *name = s_squadAges[i].name;
    *age = s_squadAges[i].age;
    return 1;
}
/* Exact CSV lookup for the original-player identity registry. Keep the
 * canonical name lookup in one place so new careers use the same CSV as
 * the squad screen and market fallback. */
int SWOS_AgesLookupExact(const char *rawName, unsigned *outAge)
{
    SquadAgeRecord key;
    SquadAgeRecord *found;
    size_t i = 0;
    if (!rawName || !outAge) return 0;
    memset(&key, 0, sizeof key);
    while (i < sizeof key.name - 1u && i < 22u && rawName[i]) {
        key.name[i] = (char)toupper((unsigned char)rawName[i]);
        ++i;
    }
    while (i && key.name[i - 1] == ' ') key.name[--i] = 0;
    if (!i) return 0;
    SquadAgeLoad();
    found = (SquadAgeRecord *)bsearch(&key, s_squadAges,
                  s_squadAgeCount, sizeof *s_squadAges, SquadAgeCompare);
    if (!found) return 0;
    *outAge = found->age;
    return 1;
}
/* Used by save loading and, later, by the seasonal-ageing mechanism.
 * Only explicit ages 15..60 are accepted. */
int SWOS_AgesSet(const char *name, unsigned age)
{
    SquadAgeRecord key, *found, *p;
    size_t i, n;
    if (!name || age < 15 || age > 60) return 0;
    n = strlen(name);
    if (!n || n >= sizeof key.name) return 0;
    SquadAgeLoad();
    memset(&key, 0, sizeof key);
    for (i = 0; i < n; ++i)
        key.name[i] = (char)toupper((unsigned char)name[i]);
    found = s_squadAgeBulkRestore ? NULL : (SquadAgeRecord *)bsearch(&key, s_squadAges,
                s_squadAgeCount, sizeof *s_squadAges, SquadAgeCompare);
    if (found) { found->age = (uint8_t)age; return 1; }
    p = (SquadAgeRecord *)realloc(s_squadAges,
                       (s_squadAgeCount + 1) * sizeof *s_squadAges);
    if (!p) return 0;
    s_squadAges = p;
    s_squadAges[s_squadAgeCount] = key;
    s_squadAges[s_squadAgeCount].age = (uint8_t)age;
    s_squadAgeCount++;
    if (!s_squadAgeBulkRestore)
        qsort(s_squadAges, s_squadAgeCount, sizeof *s_squadAges, SquadAgeCompare);
    return 1;
}
/* Advance the name-indexed career age snapshot exactly once when called
 * by ContinueCareerNextSeason. Do not call during new-career initialisation
 * or loading an existing save. The JSON save persists these updated ages.
 * Transitional support only: generated TRIAL/RES identities are NOT tracked. */
void SWOS_AgesAdvanceOneSeason(void)
{
    size_t i;
    SquadAgeLoad();
    for (i = 0; i < s_squadAgeCount; ++i) {
        if (s_squadAges[i].age < 60)
            ++s_squadAges[i].age;
    }
    fprintf(stderr, "[squad-age] advanced %zu name-based ages by one year\n",
            s_squadAgeCount);
    swos_age_log("SEASON_AGE", "entries=%zu mechanism=name_based", s_squadAgeCount);
}
#endif
