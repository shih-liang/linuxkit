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

`rootwait` waits only for a block device to appear. Once that device exists,
a filesystem mount failure enters recovery rather than retrying forever.
Initialization failures also retain PID 1 and the recovery channel instead of
exiting and causing a kernel panic.

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
escape the explicitly mounted target. Absolute symlinks resolve inside that
root, just as they do after `switch_root`. Write permission follows the mounted
filesystem itself; PID 1 adds no separate write policy.

Installer and repair adapters are data rather than compiled branches. The host
provides a read-only virtiofs payload containing the selected adapter and its
source, and `nativepipe-init` invokes it with a small fixed environment.

The initramfs includes upstream static e2fsprogs binaries for ext2/3/4:
`mke2fs` (`mkfs.ext4`), `e2fsck` (`fsck.ext4`), `resize2fs`, `tune2fs`,
and `dumpe2fs`. util-linux supplies static `blkid` and `sfdisk`; the latter is
the noninteractive GPT writer used by installation adapters. BusyBox supplies
the recovery shell, basic adapter
commands, `mount`/`umount`, and the initramfs-specific `switch_root`; its `tc`,
`blkid`, `fsck`, and `mkfs.ext2` applets are deliberately disabled.
Its `udhcpc` client and a fixed lease hook provide networking during an
installation. Ubuntu Base and Fedora Container Base are root filesystems, not
bootable disk images, so their adapters use that network to install the
distribution-specific systemd, device-manager and account baseline inside the
new root before handoff. Target runtime mounts are always unwound, and an
interrupted install can rewrite the installer-owned disk on retry only after
all its mounts have been released. A busy mount is an error, not permission to
continue formatting.

Disk growth preserves the existing partition start, identity and contents:
`sfdisk -N 1` receives `size=+`, then `e2fsck` and `resize2fs` grow ext4.
An empty size field would retain the old partition size. `sfdisk` itself
notifies the kernel; no duplicate `partprobe` or size polling is needed.

Arch Linux ARM initializes and populates the rootfs's pacman keyring before
installing packages. Its temporary GPG agent is stopped before the chroot
runtime mounts are released. Init supplies `/dev/fd` and the standard I/O links
after mounting devtmpfs, so Bash process substitution works in those chroots.
It mounts devpts for package-manager PTYs and moves both `/run` and `/tmp` into
the installed root at handoff.

`../adapters/catalog.json` is the installation source of truth consumed by the
LightHouse creation assistant. It identifies the rootfs archive, checksum,
adapter and selectable software for Ubuntu, Fedora and Arch Linux ARM. The
release workflow validates that catalog and publishes the complete adapter
directory next to the kernel and initramfs. Adapters receive only the fixed
`NP_*` environment documented above; they create the GPT/ext4 root, unpack and
initialize the selected distribution, and install selected software in the
same chroot phase. Software failures therefore return through the init recovery
channel before handoff; no first-boot installation service or completion marker
blocks guestd. Rosetta's mount/registration remains a normal boot service.
Ubuntu additionally offers an
experimental Steam choice using FEX for the client's 32-bit and 64-bit x86
code. Its Wine choice instead installs Ubuntu's amd64 Wine loader and libraries;
Apple's Virtualization translation layer runs that x86-64 process inside the
ARM64 guest.

`DEPENDENCY_VERSIONS` pins BusyBox, e2fsprogs, util-linux, and the Kata
configuration revision with source checksums. The weekly upstream workflow
updates that file and `KERNEL_VERSION` together in one pull request and rejects
a Kata configuration from a different Linux series; BusyBox `x.y.0`
development releases are excluded until an upstream stable bug-fix release
exists.

## Independent GitHub builds

`kernel-image.yml` builds only the ARM64 16 KiB kernel and its configuration,
symbols and module ABI files. `build-initramfs.yml` independently builds and
tests the static recovery tools; it does not download or compile Linux. Either
workflow can be run manually to obtain its own Actions artifact.

`build-kernel.yml` assembles their outputs into the existing paired release
format. A new kernel tag builds both jobs in parallel. For an initramfs-only
change, run the publication workflow with a new `release_tag` and an existing
`kernel_release`, for example:

```sh
gh workflow run build-kernel.yml --ref main \
  -f release_tag=lighthouse-kernel-v6.18.48-8 \
  -f kernel_release=lighthouse-kernel-v6.18.48-4
```

Kernel reuse requires the same Linux version, `config-aarch64` and Kata
configuration revision, and verifies the downloaded kernel artifacts against
the release checksums. Leave `kernel_release` empty to rebuild Linux. Neither
path builds the kernel or initramfs on the local macOS host.
