#ifndef NATIVEPIPE_CONSOLE_CONFIG_H
#define NATIVEPIPE_CONSOLE_CONFIG_H

#include <stddef.h>

#define NP_CONSOLE_ARGUMENT "console=hvc0"

enum np_console_method {
    NP_CONSOLE_METHOD_GRUB = 1u << 0,
    NP_CONSOLE_METHOD_LOADER_ENTRY = 1u << 1,
    NP_CONSOLE_METHOD_KERNEL_CMDLINE = 1u << 2,
    NP_CONSOLE_METHOD_EXTLINUX = 1u << 3,
    NP_CONSOLE_METHOD_INITTAB = 1u << 4,
};

struct np_console_result {
    unsigned detected;
    unsigned configured;
    unsigned changed;
};

/*
 * Persist console=hvc0 in the boot configuration rooted at `root`.
 *
 * `root` is normally "/".  Keeping it explicit makes the file transformation
 * testable against fixture trees without mounting or modifying a real /boot.
 * Bootloader regeneration and init-system commands are deliberately left to
 * guestd; this function only performs idempotent, atomic file updates.
 */
int np_console_configure_files(const char *root, struct np_console_result *result);

/* Apply only the adapters selected by the environment catalog. */
int np_console_configure_files_selected(const char *root, unsigned methods,
                                        int disable_unavailable_gettys,
                                        struct np_console_result *result);

/* Return 1 when an active (non-comment) line contains the exact argument. */
int np_console_path_has_hvc0(const char *root, const char *absolute_path);

#endif
