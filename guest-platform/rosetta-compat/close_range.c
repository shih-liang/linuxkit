/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "../common/fd_range.h"

int close_range(unsigned first, unsigned last, int flags) {
    return np_close_range(first, last, flags);
}
