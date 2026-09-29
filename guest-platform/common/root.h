#ifndef NP_INSTALL_ROOT_H
#define NP_INSTALL_ROOT_H

#include <stddef.h>
#include <sys/types.h>

/* All target paths are resolved inside root, including absolute symlinks in
 * usr-merged distributions. Reuse the file RPC's openat2 implementation. */
int np_root_mkdir(int root, const char *path, unsigned mode);
int np_root_write(int root, const char *path, const void *data, size_t length, unsigned mode);
int np_root_copy(int root, const char *path, int source, unsigned mode);
ssize_t np_root_read(int root, const char *path, char *data, size_t capacity);
int np_root_link(int root, const char *path, const char *target);
int np_root_unlink(int root, const char *path);
int np_root_run(int root, char *const argv[], const void *input, size_t length);
/* Isolate mounts and reopen the same directory in the new namespace. Returns
 * an owned descriptor; the caller still owns its original descriptor. */
int np_root_isolate(int root);
int np_root_bind(int root, const char *path, const char *source);

#endif
