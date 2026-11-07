/* V3 Phase 1 diagnostic-only international career trace.
 * Logging starts automatically when the game runs.
 * Appends to swos_national_trace.log in the process working directory.
 * No game state is changed. The log is created on the first trace event.
 * Remove this header and its call sites after the lifecycle audit.
 */
#ifndef SWOS_NATIONAL_TRACE_H
#define SWOS_NATIONAL_TRACE_H
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
static inline void SWOS_NationalTrace(const char *event, const char *fmt, ...)
{
    FILE *f = fopen("swos_national_trace.log", "a");
    if (!f) return;
    fprintf(f, "NATTRACE event=%s ", event);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}
#endif
