# LightHouse guest-platform release notices

The platform release contains project-original files and statically linked
toolchain components. This directory records both without assigning an
inferred open-source license to the project-original work.

- `LightHouse-Original.txt` identifies original LightHouse material as
  `LicenseRef-LightHouse-Original`. It is an all-rights-reserved provenance
  notice, not an open-source grant.
- `Zig-MIT.txt` is the unmodified Zig 0.16.0 license text. Zig supplies the
  cross compiler and compiler runtime used by the release build.
- `musl-COPYRIGHT.txt` is the unmodified musl notice bundled with Zig 0.16.0.
  The guest executables are statically linked against that musl toolchain.
- `source-inventory.json` maps platform sources and release binaries to those
  notices. The release workflow validates this inventory before publishing.

The exact Zig and musl notice files were taken from the pinned Zig 0.16.0
toolchain. Their SHA-256 values are recorded in `source-inventory.json` so a
toolchain upgrade must deliberately update the notices and inventory.

Release archives place all platform notices under
`LICENSES/linuxkit-platform/`. The component namespace keeps a merged
platform+display runtime free of duplicate license paths while this repository
continues to keep its authoritative inventory at `LICENSES/source-inventory.json`.
