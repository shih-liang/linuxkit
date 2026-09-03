#ifndef NATIVEPIPE_MDEV_CONFIG_H
#define NATIVEPIPE_MDEV_CONFIG_H

/*
 * Guard the known BusyBox mdev virtio-port symlink rule against the unnamed
 * console port exposed by VZVirtioConsoleDeviceSerialPortConfiguration.
 *
 * `root` is "/" in production and a temporary fixture root in tests. Missing
 * or unrecognised mdev configuration is a no-op. `changed` is set only after
 * the updated file has been committed atomically.
 */
int np_mdev_guard_unnamed_virtio_ports(const char *root, int *changed);

#endif
