#ifndef NP_OS_RELEASE_H
#define NP_OS_RELEASE_H
#include <stddef.h>

/* Parse os-release as data. Expansion, substitution and evaluation are never
 * performed. Returns 1 for a value, 0 if absent and -1 if malformed/too large. */
int np_os_release_value(const char *text, const char *key, char *out, size_t capacity);
#endif
