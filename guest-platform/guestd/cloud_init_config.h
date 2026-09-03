#ifndef NATIVEPIPE_CLOUD_INIT_CONFIG_H
#define NATIVEPIPE_CLOUD_INIT_CONFIG_H

/*
 * Remove only zero-byte per-instance network-config.json cache files.
 * Returns the number removed, or -1 on an unexpected traversal error.
 */
int np_cloud_init_remove_empty_network_cache(const char *root);

#endif
