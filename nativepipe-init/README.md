# nativepipe-init

`nativepipe-init` is the static PID 1 in the LightHouse initramfs. In a normal
boot it preserves the distro's standard `root=`, `rootfstype=`, `rootflags=`,
`ro`/`rw`, `rootwait`, `rootdelay=`, and `init=` semantics while mounting the
target at `/newroot`, then runs `switch_root`; this successful path never
creates a vsock socket. `UUID=`, `LABEL=`, and GPT `PARTUUID=` are resolved by
the static util-linux `blkid`. The kernel command line therefore selects the
path without a host round trip: a valid `root=` selects normal boot, while
`nativepipe.maintenance=1`, an invalid command line, or a failed root handoff
selects recovery. `PARTLABEL=` is supported as well, and a comma-separated
`rootfstype=` list retains the kernel's normal fallback behavior. It never
guesses a disk and never enters a shell automatically.

For direct LightHouse boots, the host also supplies
`nativepipe.memory_target_bytes=<bytes>`. PID 1 validates the decimal value and
writes it to `/run/nativepipe/target-memory-bytes`; `/run` is then moved into
the real root. The value is informational inside the guest. The host applies
the same value to Virtualization.framework's memory-balloon device after the
VM starts, which is the operation that actually changes the guest allowance.

Only recovery opens virtio-vsock. The init connects to host CID 2, port 1024,
and immediately sends the existing `NPRT` ready handshake with the
`init.control` capability. That accepted socket then carries the ordinary
version-1 `NPIP` control protocol: little-endian request IDs and the same
`NPOK`/`NPER`, `NPRE`/`NPFL`/`NPLS`, `NPMS`/`NPFS`, and `NPWR` filesystem
operations. It retries the outbound connection if the host is not ready and
closes it before `switch_root`; the installed `nativepipe-guestd` subsequently
listens on guest port 1024 and the host connects in the normal direction.

Init-only operations are:

- `NPIH`: enumerate block devices and their VZ block identifiers.
- `NPIM`: mount the exact identifier and partition selected by the host.
- `NPIC`: boot, install, repair, or start a console shell.

For boot/install/repair, `NPOK` is sent only after the adapter completed, the
root and its ELF/script interpreter passed preflight, and all runtime mounts
were moved successfully. Failures before that point return `NPER`; partial
mount moves are rolled back before recovery resumes.

Paths presented to the host are ordinary absolute guest paths. During early
boot they are resolved beneath `/newroot` with `openat2`, so a path cannot
escape the explicitly mounted target. Write permission follows the mounted
filesystem itself; PID 1 adds no separate write policy.

Installer and repair adapters are data rather than compiled branches. The host
provides a read-only virtiofs payload containing the selected adapter and its
source, and `nativepipe-init` invokes it with a small fixed environment.

The initramfs includes upstream static e2fsprogs binaries for ext2/3/4:
`mke2fs` (`mkfs.ext4`), `e2fsck` (`fsck.ext4`), `resize2fs`, `tune2fs`,
and `dumpe2fs`. util-linux supplies the static `blkid` needed for filesystem
and partition identifiers. BusyBox supplies the recovery shell, basic adapter
commands, `mount`/`umount`, and the initramfs-specific `switch_root`; its `tc`,
`blkid`, `fsck`, and `mkfs.ext2` applets are deliberately disabled.

`DEPENDENCY_VERSIONS` pins BusyBox, e2fsprogs, util-linux, and the Kata
configuration revision with source checksums. The weekly upstream workflow
updates that file and `KERNEL_VERSION` together in one pull request and rejects
a Kata configuration from a different Linux series; BusyBox `x.y.0`
development releases are excluded until an upstream stable bug-fix release
exists.
