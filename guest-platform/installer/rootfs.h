#ifndef NP_INSTALL_ROOTFS_H
#define NP_INSTALL_ROOTFS_H

#include "distro.h"

/* target must be an owned, unmounted rootfs directory. Numeric ownership is
 * preserved on Linux. The implementation never follows archive path escapes. */
int np_rootfs_extract(const char *source, const char *target,
                     enum np_rootfs_format format, const char *architecture);
int np_rootfs_extract_at(const char *source, int target,
                        enum np_rootfs_format format, const char *architecture);

#endif
