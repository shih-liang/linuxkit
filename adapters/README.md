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
