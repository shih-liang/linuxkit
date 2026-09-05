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
