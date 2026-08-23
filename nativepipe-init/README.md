# nativepipe-init

`nativepipe-init` is the static PID 1 in the LightHouse initramfs. In a normal
boot it mounts the explicit kernel `root=` at `/newroot` and immediately runs
`switch_root`; it does not open a control listener. With the explicit
`nativepipe.maintenance=1` kernel flag it mounts `devtmpfs`, `/proc`, `/sys`,
`/run`, and `/tmp`, then waits for a host request. It never guesses a disk and
never enters a shell automatically.

The process listens on virtio-vsock port 1024, the same control endpoint used
by the installed `nativepipe-guestd`. Both stages use version-1 `NPIP` frames,
little-endian request IDs, and the same `NPOK`/`NPER`, `NPRE`/`NPFL`/`NPLS`,
`NPMS`/`NPFS`, and `NPWR` filesystem operations. `NPHI` reports an
`init.control` capability while the initramfs is active. `switch_root` closes
the listener; the host then reconnects to guestd on the same port.

Init-only operations are:

- `NPIH`: enumerate block devices and their VZ block identifiers.
- `NPIM`: mount the exact identifier and partition selected by the host.
- `NPIC`: boot, install, repair, or start a console shell.

Paths presented to the host are ordinary absolute guest paths. During early
boot they are resolved beneath `/newroot` with `openat2`, so a path cannot
escape the explicitly mounted target. Write permission follows the mounted
filesystem itself; PID 1 adds no separate write policy.

Installer and repair adapters are data rather than compiled branches. The host
provides a read-only virtiofs payload containing the selected adapter and its
source, and `nativepipe-init` invokes it with a small fixed environment.
