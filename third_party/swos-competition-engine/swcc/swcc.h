/* swcc library API (ctypes from Python; WASM later). */
#ifndef SWCC_H
#define SWCC_H

#include <stddef.h>

/* Validates spec (JSON text, schema swc/1) against profile (JSON text, a host's limits). Returns the number of
   errors. diag_out: every diagnostic, one per line, NUL-terminated (free with swcc_free). When there is no error and
   bin is not NULL, *bin / *blen receive the binary spec (free with swcc_free). */
int  swcc_compile(const char *spec, size_t len, const char *name, const char *profile, size_t plen,
                  unsigned char **bin, size_t *blen, char **diag_out);
void swcc_free(void *p);

#endif
