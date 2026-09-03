# Guest platform licensing

The authoritative release inventory and notices are stored at the repository
root in [`LICENSES/`](../../LICENSES). Project-original files remain
`LicenseRef-LightHouse-Original` (all rights reserved; no inferred open-source
grant). Static release binaries additionally retain the exact Zig 0.16.0 MIT
license and musl COPYRIGHT notice from the pinned toolchain.

Run `python3 guest-platform/scripts/check-license-inventory.py` before a
release. Publication remains fail-closed if the inventory or either pinned
third-party notice is missing or changed.
