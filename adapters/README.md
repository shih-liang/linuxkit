# Installation catalog

New FluxWindow installations use the modular C implementation in
`guest-platform/installer`. Both `nativepipe-init` and `nativepipe-install` are
built into the recovery initramfs. The host sends a data plan and guest artifacts;
recovery starts the installer directly, without a shell bridge. The old `.sh`
adapters are retained for older saved installation requests and are not bundled
by new FluxWindow builds. See `guest-platform/INSTALLATION.md` for ownership and
the recovery/payload lifecycle.

`catalog.json` is stable distribution and discovery policy. Each source's
`versionSeries` pins an approved release line: Ubuntu **26.04**, Debian **13**,
Fedora **44**, and Alpine **3.24**. Patch releases inside that line remain
discoverable. Advancing to another line requires installation/runtime testing
and a catalog revision. ARM64 and optional amd64 sources carry the same policy.
Artifact URLs and digests still come from the publisher, not a generated
`latest` record in this repository.

Alpine uses the explicit `v3.24` directory instead of `latest-stable`. Debian
uses `trixie/` instead of `stable/`, while continuing to resolve all metadata at
one immutable publisher commit. Arch Linux ARM is rolling and has no stable
major release: its installation image is pinned to the tested **2026.08**
snapshot. Arch package repositories still roll, so this image pin does not
freeze package versions or promise future package compatibility.

When the creation marketplace opens, FluxWindow reads each publisher-owned
`indexURL`, selects matching ARM64 artifacts, obtains the publisher's checksum,
and caches the resolved releases in its own Application Support directory. New
results replace the `Latest` marker within the approved line. Both discovery
and cache loading enforce the same policy, including an optional amd64 rootfs;
cached releases outside the line are no longer offered for new installations.
Existing VM disks are unaffected. FluxWindow verifies the downloaded rootfs before installation, and
the C installer checks it again before preparing the disk. Debian's published
rootfs, version and manifest are resolved at the same immutable Git commit.

The kernel/dependency update workflow must never read or modify this directory.
Only a change to catalog metadata, an upstream discovery format, or trust policy
requires a catalog revision and a new `catalog.signed.json`. The bundled catalog
is covered by the application signature; the remote catalog retains its existing
Ed25519 trust policy. Installer changes ship in a new recovery image and do not
rewrite unchanged catalog bytes.

Guest GUI dependencies come from the selected distribution. Fedora and Arch
install both Xwayland and `xwayland-satellite`; Ubuntu installs Xwayland and
enables satellite only when that package appears in its configured archive.
Neither linuxkit nor NativePipe carries a private satellite executable.

All package installation, including optional software, completes inside the
installer's chroot before handing off to the real init. The installer reads the
payload from a read-only, noexec share; package operations use private mount and
PID namespaces. A failed package transaction leaves the task in recovery with
its installation identity and stage available for retry. No first-boot software
service is installed.

The baseline includes user-session D-Bus/PAM integration, desktop settings
schemas, fonts and icons even when no optional software is selected. Fedora
uses the distribution's `libwayland-client` and `libwayland-server` packages;
there is no `wayland-libs` package.

Arch installs the distribution's `realtime-privileges` package and adds the
account selected in FluxWindow to its `realtime` group. The package owns the
group and PAM limits; the adapter does not duplicate them or hardcode a UID.
Arch's `systemd-user` PAM stack loads these limits for the user manager and its
audio services, allowing PipeWire's `libpipewire-module-rt` to set realtime
priorities directly. These permissions also apply to other processes inheriting
that user's PAM session limits, not just audio processes.

Ubuntu and Fedora retain the distribution's ordinary-user scheduling policy.
Their adapters do not create a `realtime` group, add users to that group, or write
custom PAM limits. PipeWire uses the distribution's packaged realtime module and
RTKit policy; existing PAM, realtime-priority, nice and memory-locking limits are
unchanged. Arch's explicit group selection is not applied to other adapters.

All adapters explicitly install the distribution's `rtkit` package as a fallback.
Its authorization still depends on the guest session meeting the distribution's
policy. Installing RTKit alone is not proof that realtime scheduling works.
The audio stack includes the distribution's ALSA-to-PipeWire default routing
(`pipewire-alsa` on Arch and Fedora, supplied by `pipewire-audio` on Ubuntu).
ALSA applications then share the session audio server instead of opening the
single hardware playback stream exclusively.

After creating the selected account and installing its software, all adapters
enable `pipewire.service`, `pipewire-pulse.service` and `wireplumber.service`
with `SYSTEMD_OFFLINE=1 systemctl --user --no-reload enable`, executed as that
user. This only writes the user's service enablement links; no audio daemon or
user D-Bus is started in the installer chroot. The packaged user services start
with the user manager, rather than waiting for the first application's audio
request. On systemd guests NativePipe does not launch a competing set of audio
daemons. Fedora installs `util-linux`, not just `util-linux-core`, because
`runuser` is packaged in the former.

These are fresh-install defaults, not a migration for existing VMs. Changing
limits and group membership in an existing VM requires recreating the user
manager and audio processes; logout alone may leave a lingering manager running.

The FluxWindow kernel retains `CONFIG_RT_GROUP_SCHED=y`. The host adds
`rt_group_sched=0` to its direct-boot defaults because cgroup v2 has no interface
to provision the legacy per-group realtime budget, even when RTKit is installed.
This boot parameter works with the existing 6.18 kernels; no kernel rebuild is
required. It leaves normal cgroup CPU limits and the global realtime CPU budget
enabled. Explicit extra boot arguments can override the default. EFI boot uses
the guest bootloader's own command line. Changed boot arguments take effect on
the next VM boot, not by restarting the audio services.

## Ubuntu amd64 environment

Ubuntu offers an optional `amd64-rootfs` component. The native ARM64 root still
owns init, guestd, the compositor, devices and services. A separate Ubuntu
amd64 userspace lives at `/var/lib/nativepipe/amd64`, with its own libraries and
package database. No x86 kernel or x86 PID 1 is booted.

The catalog's `amd64Source` uses the same publisher discovery/checksum policy as
the base source. FluxWindow pairs images by exact release version and verifies
each archive independently. Failure to find an amd64 image leaves native releases
available but prevents choosing the additional environment. Both downloads move
into the installation payload without retaining second copies.

Rosetta requires the **4 KiB ARM64 kernel**: ordinary Ubuntu amd64 ELF programs
fail to map under the 16 KiB kernel. FluxWindow selects and pins the Rosetta
variant for new installations using Rosetta; adding it does not change existing
VMs' automatic 16 KiB kernel selection. The boot header is checked on the Mac,
and the shared C Rosetta module checks the actual guest page size before disk
preparation.
The release workflow builds both variants from one device/security configuration
plus `config-rosetta-aarch64`, and publishes one shared ARM64 initramfs.

`guest-platform/common/rosetta.c` owns the virtiofs mount and the x86-64-only binfmt
registration, shared by recovery installation and the normal guest service.
The C installer owns extraction and package operations for the separate userspace;
`installer/amd64.c` configures the standard distribution `schroot` entry. Shared
root/account modules retain namespace cleanup and account policy.
Runtime sessions, user authorization, terminal handling and mount teardown are
provided by schroot; FluxWindow reuses its existing VM terminal/exec transport.

NativePipe owns GPU page-size compatibility. Its preload library handles both
legacy and extended virtio-gpu blob ioctls, and its Vulkan layer aligns host-visible
allocations before export. FluxWindow packages the ARM and amd64 builds; the
installer puts the amd64 libraries inside the additional rootfs. The schroot
entry discards inherited ARM loader settings and loads only amd64 libraries.
Recursive slave mounts include systemd's per-user tmpfs and nested shared folders;
schroot tears down these session mounts without unmounting the native sources.

After installation, choose **amd64 Shell** in the terminal workspace, or run:

```sh
fluxwindow -d Ubuntu nativepipe-amd64
fluxwindow --no-pty -d Ubuntu nativepipe-amd64 dpkg --print-architecture
fluxwindow -d Ubuntu nativepipe-amd64 sudo apt-get install <package>
fluxwindow -d Ubuntu nativepipe-amd64 <application> <arguments...>
```

The selected VM account keeps its UID and password-protected sudo policy.
Home directories, VM shared folders and user Wayland/X11/audio/D-Bus sockets
remain accessible. The native system manager socket is not shared. Package
scripts cannot start a second init; an amd64 package manager updates only the
amd64 package database. This is a compatibility environment for trusted VM
applications, not a security boundary from the native VM. Rosetta translates
x86-64 applications; 32-bit x86 software still needs an appropriate translator.
