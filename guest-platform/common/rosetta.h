#ifndef NP_ROSETTA_H
#define NP_ROSETTA_H

#include <stddef.h>

/* Shared by recovery installation and the native boot service. */
int np_rosetta_prepare(void);
/* Pure validation, also used by tests; never replace somebody else's handler. */
int np_rosetta_handler_matches(const char *contents);
const char *np_rosetta_registration(void);

#endif
