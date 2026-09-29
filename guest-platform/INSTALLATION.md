# Installation and guest ownership

FluxWindow's schema-4 installation entry uses the C installer bundled inside
the recovery initramfs. `nativepipe-init` executes `/sbin/nativepipe-install`
directly; the host does not create or send an installation launcher script.
The legacy adapter protocol remains only to resume VMs created by older builds.
Keeping that compatibility path does not make it the entry for new installs.

## Boot and userspace architecture

Apple Silicon VMs boot an ARM64 kernel. Native ARM64 init, guestd, session and
compositor retain ownership of the VM's devices and host connection. A 4 KiB
kernel is required whenever Rosetta is enabled. amd64 root filesystems provide
translated userspace, not an x86 kernel, driver ABI or a second booted VM.

An amd64 environment has its own distribution packages, dynamic loader and
account database. Only the interactive account's identity is matched to the
native system; copying the whole native passwd/group database would overwrite
distribution service accounts. User directories and session sockets are shared
explicitly. ARM preloads must be removed before entering amd64; graphics hooks
must match both the target architecture and libc.

## Responsibilities

* FluxWindow discovers upstream releases, authenticates the installation
  catalog, checks artifact digests, and fixes concrete artifacts for each
  installation. The catalog pins the tested release series for discovery and
  cached results; the installer keeps the selected rootfs's package repositories.
  It owns download cancellation and the unpublished VM bundle.
* `nativepipe-init` owns recovery boot, disk identity, host installation requests
  and switching to the installed root. Distribution policy does not belong in
  PID 1. It advertises `init.install.rootfs.c.v1` only in the recovery image
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
  audio ownership. The native distribution's init system supervises them.

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
8. Release installation resources, then boot the native system. Runtime guestd
   reports readiness independently of installer completion.

FluxWindow pins a copy of the bundled recovery image to the pending operation,
next to (outside) its read-only payload. Normal boots retain the VM's selected
kernel/initramfs pair. The payload contains `install.json`, verified source
archives, account input and guest artifacts. It contains no adapter, common
script or launch bridge. After the installed guestd connects, FluxWindow removes
the account input and both source archives; the plan and recovery image remain
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
OpenRC entry or compatibility launcher may remain a script when the surrounding
interface requires it. Interactive sessions execute the selected account's
default shell, rather than forcing `/bin/sh`.

## Release discovery and validation

"Latest" is discovered from upstream, not hardcoded as a moving archive URL.
Each installation records an immutable version/artifact/digest selection. Arch
Linux and Arch Linux ARM have separate publishers and release numbering; they
must not be paired by an assumed common release string. An unavailable optional
amd64 image must not hide an otherwise usable native release.

Validation covers the latest stable Alpine, Debian, Ubuntu, Fedora and Arch
families, for native ARM64 and translated amd64. Tests must include malformed
plans/archives, interrupted installation, package failure and mount cleanup,
then actual installation, reboot, default-shell PTY, guest communication and
graphics. Report those results separately: archive extraction or a successful
build alone does not establish that a distribution works.
