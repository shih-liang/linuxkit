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
Only a change to adapter behavior, an upstream discovery format, or trust policy
requires a catalog revision and a new `catalog.signed.json`.
