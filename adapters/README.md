# Installation adapters

`catalog.json` is stable adapter and discovery policy. It must not contain a
specific release URL, release version, MD5/SHA value, or a generated `latest`
record. The only version-like value allowed here is a compatibility floor that
states which upstream releases an adapter can install.

When the creation marketplace opens, LightHouse reads each publisher-owned
`indexURL`, selects matching ARM64 artifacts, obtains the publisher's checksum,
and caches the resolved releases in its own Application Support directory. New
results replace the `Latest` marker while older cached releases remain
selectable. LightHouse verifies the downloaded rootfs before installation.

The kernel/dependency update workflow must never read or modify this directory.
Only a change to catalog metadata, an upstream discovery format, or trust policy
requires a catalog revision and a new `catalog.signed.json`. Adapter script
changes ship with a new linuxkit/FluxWindow build and do not rewrite unchanged
catalog bytes.

Guest GUI dependencies come from the selected distribution. Fedora and Arch
install both Xwayland and `xwayland-satellite`; Ubuntu installs Xwayland and
enables satellite only when that package appears in its configured archive.
Neither linuxkit nor NativePipe carries a private satellite executable.

All package installation, including optional software, completes inside the
installer's chroot before handing off to the real init. The payload is available
as a read-only bind mount during those commands and is not a boot-time
dependency. A failed package transaction returns to recovery rather than
preventing the installed guestd from starting. No first-boot software service
or completion marker is installed.

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
