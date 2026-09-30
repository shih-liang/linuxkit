# Rosetta systemd support

The ARM64 recovery image includes a small amd64 shared library for translated
systemd roots. The C installer copies it to
`/usr/libexec/nativepipe/libnativepipe-rosetta.so`. It has no `DT_NEEDED` entries:
all libc calls resolve against the process's existing libc.

Early init loads it only when the selected amd64 init resolves to the installed
`/usr/lib/systemd/systemd` inode. A `user@.service` drop-in also loads it for user
managers. systemd's executors inherit their manager's environment. Four named
drop-ins cover `systemd-journald.service`, `systemd-userdbd.service`,
`systemd-udevd.service` and `systemd-vconsole-setup.service`, whose
internal work also needs these interfaces. No wildcard applies to other
systemd services. The D-Bus broker was separately
verified to work without this library once journald recovered. Ordinary unit
payloads use their own environment. The user-manager configuration explicitly
clears `LD_PRELOAD` from its default payload environment. There is no global
`/etc/ld.so.preload` file and no application-facing compatibility switch.

Device rules start virtual-console setup during cold boot. Its fork path needs
the descriptor fallback before executing `loadkeys` or `setfont`: direct
`loadkeys` works on Rosetta, while systemd's launch fails without the library.
Keep this service operational rather than skipping keyboard/font setup.

Two syscall gaps are handled:

- Rosetta lacks `faccessat2`; libc's fallback rejects `AT_EMPTY_PATH`. When the
  original check fails with EINVAL/ENOSYS, the library checks the pinned regular
  file/directory through `/proc/self/fd/N`. Kernel access checks, real/effective
  identity selection and permission denials remain authoritative. Pinned
  symlinks, unknown flags and unrelated errors do not take this fallback.
- Rosetta lacks `close_range`. The shared `common/fd_range.h` implementation,
  also used by guestd, enumerates open descriptors without allocating after
  fork. It closes or marks only the requested range, preserves excluded
  descriptors, validates flags and performs requested file-table unsharing.
  Missing procfs or denied operations fail closed. It does not scan a possibly
  huge file-descriptor limit.

The installer separately sets `MemoryDenyWriteExecute=no` for translated system
and user services because Rosetta needs to generate executable translated code.
Other unit restrictions stay in place. Native ARM64, native amd64 and OpenRC
roots do not receive these systemd settings.

Build and run access-check regression tests on amd64 Linux with `make all test`.
The common descriptor tests exercise real and injected syscall failures on
native Linux; `--native-only` runs the real fallback on Rosetta, which cannot
install the seccomp filters used for error injection. Runtime acceptance also
requires an actual installation, service/terminal/network/graphics checks and
cold boot; a successful library test alone does not establish guest usability.
