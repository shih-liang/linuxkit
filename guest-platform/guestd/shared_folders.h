#ifndef NATIVEPIPE_SHARED_FOLDERS_H
#define NATIVEPIPE_SHARED_FOLDERS_H

#define NP_HOST_SHARE_TAG "lighthouse-shares"
#define NP_HOST_SHARE_MOUNT "/mnt/lighthouse"

/* Idempotent, host-requested transition. Never force/lazy-unmount busy files. */
int np_shared_folders_set_mounted(int mounted);

#endif
