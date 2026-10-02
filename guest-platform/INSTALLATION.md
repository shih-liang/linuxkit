# Installation and guest ownership

LinPortal's schema-5 installation entry uses the C installer bundled inside
the recovery initramfs. `nativepipe-init` executes `/sbin/nativepipe-install`
directly; the host does not create or send an installation launcher script.

## Boot and userspace architecture

Apple Silicon VMs boot an ARM64 kernel. The installation plan independently
selects the root userspace ABI. Every amd64 operating-system choice uses amd64 init, guestd, session,
compositor and distribution packages, under Rosetta on a 4 KiB ARM64 kernel.
There is one root, one account database, and one set of services. No x86 kernel,
driver ABI, nested environment or separate terminal transport is introduced.

The ARM64 initramfs validates the init executable and its interpreter chain,
prepares Rosetta, then switches to the amd64 root. The `/run/rosetta` mount moves
with `/run`; binfmt's F flag pins the interpreter through the root transition.
On amd64 roots the installer masks `systemd-binfmt.service`: its default start
and stop operations clear all handlers, which would remove the execution path
of PID 1 and every service. Early init owns Rosetta for the whole VM lifetime;
additional binary formats can still be registered individually through binfmt_misc.
Its automount unit is also masked because early init already mounted binfmt_misc.
LinPortal retains this initramfs after installation and selects the runtime
publish directory by the VM's root ABI, including self-updates.

amd64 roots require the current Rosetta adaptations: systemd's execution policy
and scoped preload library are described in
[`rosetta-compat/README.md`](rosetta-compat/README.md). Arch's C installer disables
pacman filesystem and syscall sandboxing because Rosetta does not implement the
required syscalls. These runtime requirements are independent of saved host
configuration or installation-task formats.

## Responsibilities

* LinPortal discovers upstream releases, authenticates the installation
  catalog, checks artifact digests, and fixes concrete artifacts for each
  installation. The catalog pins the tested release series for discovery and
  cached results; the installer keeps the selected rootfs's package repositories.
  It owns download cancellation and the unpublished VM bundle.
* `nativepipe-init` owns recovery boot, disk identity, host installation requests
  and switching to the installed root. Distribution policy does not belong in
  PID 1. It advertises `init.install.rootfs.c.v2` only in the recovery image
  shipped with the C installer.
* `nativepipe-install` is a finite C operation. It owns disk preparation, rootfs
  extraction, package installation, initial account and service configuration,
  verification, and cleanup. It is not a daemon or a package manager.
* Distribution adapters describe package names, package-manager operations and
  the small number of distribution-specific setup operations. Debian/Ubuntu
  share APT operations; Alpine, Fedora and Arch use APK, DNF and Pacman.
* Shared guest C modules handle filesystem/process operations, environment
  discovery, Rosetta, account and service configuration. Consumers link the
  modules they use; there is no plugin loader or network-executable policy.
* `nativepipe-guestd` owns the existing host protocol, terminal/file services,
  guest integration and runtime artifact updates. Installation requests do not
  become a second implementation of those protocols.
* `nativepipe-session` and the compositor retain desktop, input, graphics and
  audio ownership. The root distribution's init system supervises them.

## Installation operation

1. Validate the plan, selected architecture, target disk identity, archive and
   required payloads. Check 4 KiB pages and Rosetta before destructive work.
2. Prepare only the selected new disk. Record enough state to distinguish a
   fresh disk from an interrupted installation; a retry must not implicitly
   format an already prepared disk.
3. Extract the verified rootfs inside its owned target directory. Honor the
   archive format, numeric IDs, symlinks and relevant metadata. Reject path
   escapes. OCI images are selected by manifest and architecture, with layers
   applied in order and whiteouts processed according to the OCI specification.
4. Run the distribution package manager with explicit argv and a controlled
   environment. Chroot operations use private mount and PID namespaces; the
   operation owns and reaps its children. Do not expose the native system
   manager's `/run` to package scripts.
5. Create the requested account, preserve a valid existing default shell, lock
   upstream default logins and apply the distribution's administrator policy.
   Passwords go through dedicated input, never argv or logs.
6. Install guest artifacts and configure native networking, init services,
   console, user audio and optional Rosetta. OpenRC/systemd are shared adapters,
   not copied blocks in every distribution implementation.
7. Verify rootfs architecture, package operations, init, account and executable
   artifacts. Persist completion only after successful verification and sync.
8. Release installation resources, then boot the installed system. Runtime guestd
   reports readiness independently of installer completion.

LinPortal pins a copy of the bundled recovery image to the pending operation,
next to (outside) its read-only payload. ARM64 roots retain the selected
kernel/initramfs pair. amd64 roots retain the Rosetta-ready image at
`platform/rosetta-initramfs` for every normal boot. The payload contains
`install.json`, verified source
archives, account input and guest artifacts. It contains no adapter, common
script or launch bridge. After the installed guestd connects, LinPortal removes
the account input and the source archive; the plan and recovery image remain
available for an explicit disk-resize repair.

Arch's general-purpose ARM image includes a physical-machine kernel and firmware.
The C policy removes those specific packages with Pacman before the first update,
so installing a VirtIO VM does not download their updates. Headers, kmod, Mesa and
VirtIO userspace packages remain. DNF disables weak dependencies and APT disables
recommendations; package-manager dependency checks remain enabled.

Failures keep a useful stage and error. They do not claim rollback of a whole
package transaction, delete an unknown filesystem or forcibly detach mounts
still used by an unrelated process. Recovery and explicit fresh installation
are distinct operations.

## Shell boundary

Installation control flow is C. The implementation must not use `system()`,
`popen()`, shell command interpolation, or source `/etc/os-release`. Package
maintainer scripts remain the distribution's responsibility. A short standard
OpenRC service entry may remain a script when the surrounding
interface requires it. Interactive sessions execute the selected account's
default shell, rather than forcing `/bin/sh`.

## Release discovery and validation

"Latest" is discovered from upstream, not hardcoded as a moving archive URL.
Each installation records an immutable version/artifact/digest selection. Arch
Linux and Arch Linux ARM have separate publishers and release numbering; they
must not be paired by an assumed common release string. Root architectures have
independent catalog entries; an unavailable amd64 image must not hide a usable
ARM64 release.

The current install catalog covers pinned Alpine, Debian, Ubuntu, Fedora and
Arch release series for ARM64 and amd64 as ten independent operating-system
choices. Optional applications use the shared [application table](installer/APPLICATIONS.md)
and compiled distribution presets. Changing the
operating-system architecture also changes the kernel requirement and artifact
ABI; it does not install a nested environment. Tests must include malformed
plans/archives, interrupted installation, package failure and mount cleanup,
then actual installation, reboot, default-shell PTY, guest communication and
graphics. Report those results separately: archive extraction or a successful
build alone does not establish that a distribution works.
