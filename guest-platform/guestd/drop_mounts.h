#ifndef NATIVEPIPE_DROP_MOUNTS_H
#define NATIVEPIPE_DROP_MOUNTS_H

/* Serves NP_DROP_MOUNT_SOCKET: mounts the drop filesystem for the requesting
 * user and hands the /dev/fuse descriptor to that user's session. */
int np_drop_mounts_start(void);

#endif
