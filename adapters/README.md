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
and a catalog revision. ARM64 and amd64 root-system sources carry the same policy.
Artifact URLs and digests still come from the publisher, not a generated
`latest` record in this repository.

Alpine uses the explicit `v3.24` directory instead of `latest-stable`. Debian
uses `trixie/` instead of `stable/`, while continuing to resolve all metadata at
one immutable publisher commit. Arch Linux ARM is rolling and has no stable
major release: its installation image is pinned to the tested **2026.08**
snapshot; Arch Linux amd64 uses the **2026.09.01** bootstrap snapshot. The
bootstrap's `root.x86_64/` envelope is removed by the shared archive extractor,
and the Arch Linux keyring and HTTPS mirror are selected by its C policy.
Arch package repositories still roll, so this image pin does not
freeze package versions or promise future package compatibility.

When the creation marketplace opens, FluxWindow reads each publisher-owned
`indexURL`, selects artifacts for the chosen userspace architecture, obtains the publisher's checksum,
and caches the resolved releases in its own Application Support directory. New
results replace the `Latest` marker within the approved line. Both discovery
and cache loading enforce the same policy, including the amd64 root system;
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

Application names and available GUI choices come from
[`applications.json`](../guest-platform/installer/applications.json);
[`sync-apps.py`](sync-apps.py) keeps this catalog in sync. See the
[application installer](../guest-platform/installer/APPLICATIONS.md) for package
mappings, official presets and the post-install `apps` command.

All initial package installation, including optional software, completes inside the
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

## amd64 root systems

Select **Alpine Linux (amd64)**, **Debian (amd64)**, **Ubuntu (amd64)**,
**Fedora (amd64)** or **Arch Linux (amd64)** under operating systems to install
an amd64 filesystem as the VM root. Each uses its distribution packages,
account database, init system, and amd64 guestd, session and compositor.
Alpine uses BusyBox init and OpenRC; the others use systemd. There is no second
rootfs or schroot launcher. Software choices contain only Developer Tools;
architecture, Wine and Steam are not software installation options.

The VM still boots a **4 KiB ARM64 kernel**. The native initramfs validates the
amd64 init and dynamic loader, mounts the Rosetta virtiofs share, registers only
x86-64 ELF files with binfmt_misc, then moves runtime mounts and switches root.
The read-only Rosetta mount remains at `/run/rosetta`; the F flag pins its
interpreter. Translation is available before the first amd64 process starts.

`guest-platform/common/rosetta.c` is shared by recovery installation, early boot
and optional compatibility on ARM64 systems. Distribution package and service
configuration is shared by both root architectures. FluxWindow pins the updated
initramfs inside the VM for subsequent boots and requires Rosetta to stay enabled.
The host publishes runtime files by the root's ABI, including self-updates and
matching graphics hooks. The normal VM terminal and application entrypoints work
directly; the historical amd64 Shell remains only for old nested environments.

```sh
fluxwindow -d Ubuntu-amd64
fluxwindow --no-pty -d Ubuntu-amd64 dpkg --print-architecture
fluxwindow -d Ubuntu-amd64 sudo apt-get install <package>
```
