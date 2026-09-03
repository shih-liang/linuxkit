# LinuxKit guest platform

This directory is the source of the small, distribution-independent guest
platform installed by LightHouse:

- `common/` contains the shared NPAG/vsock transport implementation.
- `bootstrap/` contains the one-shot cidata bootstrap program.
- `guestd/` contains the persistent guest agent, its unit files, installer,
  distribution configuration helpers, and unit tests.

The sources were migrated from
[`shih-liang/LightHouse`](https://github.com/shih-liang/LightHouse) at commit
`4cb1e4083c795d714a5fb5950cba8fa6772e3235`. They are maintained here after
the migration; the release manifest therefore identifies this repository and
the commit that built the release.

## Build and test

Zig cross-compiles static musl binaries for both supported guest
architectures. The host tests require only a C compiler and Python 3.

```sh
make -C guest-platform test
make -C guest-platform build
make -C guest-platform test-package
```

`make package` produces an authoritative, deterministic uncompressed ustar
archive for each architecture. Core release asset names are stable
(`linuxkit-platform-<architecture>.tar`, `.runtime-manifest.json`, and
`.runtime-manifest.sig`); version and monotonic release sequence are carried by
the immutable tag and signed manifest. It also produces a deterministic gzip
copy for manual downloads and external runtime, source, license, and SPDX
metadata.
The runtime manifest describes the uncompressed archive, so it is intentionally
not stored inside that archive: embedding an archive's own size and SHA-256
would be self-referential.

Each release also carries `SHA256SUMS` and `SHA256SUMS.sig`. These are useful
for CI, mirrors, and manual downloads, but LightHouse's update trust root is the
direct Ed25519 signature over each canonical runtime manifest; the manifest's
signed archive and per-file SHA-256 values remain authoritative.

Archive entries are relative directories or regular files only. Ownership,
timestamps, modes, ordering, and gzip headers are normalized. Paths retain the
existing NativePipeRuntime repository layout, so LightHouse can merge this tar
with a display-runtime tar without a path-translation compatibility layer.
Only runtime inputs are shipped; C sources, Makefiles, and tests remain in this
public repository and are represented by separate provenance/SBOM assets. The
`InstallAdapters/` directory is the catalog and adapter set verified as part
of the same runtime release:

```text
InstallAdapters/README.md
InstallAdapters/catalog.json
InstallAdapters/catalog.signed.json
InstallAdapters/catalog-public-key.txt
InstallAdapters/*.sh
InstallAdapters/validate.py
LICENSES/linuxkit-platform/README.md
LICENSES/linuxkit-platform/LightHouse-Original.txt
LICENSES/linuxkit-platform/Zig-MIT.txt
LICENSES/linuxkit-platform/musl-COPYRIGHT.txt
LICENSES/linuxkit-platform/source-inventory.json
Resources/GuestEnvironmentCatalog.json
Resources/GuestEnvironmentCatalogPublicKey.txt
guest/bootstrap/dist/nativepipe-bootstrap-<architecture>
guest/guestd/VERSION
guest/guestd/dist/nativepipe-guestd-<architecture>
guest/guestd/install.sh
guest/guestd/nativepipe.interfaces
guest/guestd/nativepipe.modules
guest/guestd/openrc/nativepipe-guestd
guest/guestd/systemd/nativepipe-guestd.service
```

`GuestEnvironmentCatalog.signed.json` is also published beside each release
as the independently signed update envelope. It is not needed inside the
runtime archive: the archive contains the matching fallback catalog and its
pinned Ed25519 public key. The stable update URL is
`https://raw.githubusercontent.com/shih-liang/linuxkit/main/Resources/GuestEnvironmentCatalog.signed.json`;
the catalog revision check rejects rollback even though that URL is mutable.

The tag format is
`linuxkit-platform-v<VERSION>-<releaseSequence>`. `releaseSequence` is a
repository-wide monotonic integer in `1...UInt64.max`; it must not reset when
VERSION changes. Workflow comparisons operate on decimal strings rather than
Bash's signed integer arithmetic. Runtime activation is forward-only: the host
rejects a lower or equal sequence, and a failed candidate never replaces the
active set.

`releaseSequence` is also the cross-repository runtime-set generation. A
platform release is activated only with the RemotePipe display release carrying
the same positive sequence, and the bundled baseline lock uses that same value
as `setGeneration`. The two repositories do not need to publish simultaneously:
the host retains the current complete set until both independently signed
releases for one generation are available. Do not reuse a sequence for a
different platform/display pairing.

## Signing and releases

`.github/workflows/build-guest-platform.yml` runs only for matching tags. It
builds and tests both architectures and verifies deterministic packaging. The
tag commit must be reachable from `main` and must descend from the commit named
by the greatest earlier release sequence. A higher sequence therefore cannot
re-sign an older source ancestor. Signing is delegated to
`.github/workflows/sign-guest-platform.yml@main`, so the key-bearing policy is
loaded from the protected branch rather than from the tag. Its job has only
`contents: read`; it verifies the candidate and uploads a signed Actions
artifact. A separate job with `contents: write` but no private key verifies that
artifact, independently checks its public key against the pinned repository
variable, and publishes it. Every third-party GitHub Action is pinned to an
immutable commit.

These checks rely on repository controls. Configure a branch ruleset for
`main` that blocks force-push and deletion, requires pull-request review and
required CI, and requires code-owner review for `.github/workflows/**`,
`guest-platform/scripts/**`, and `scripts/sign-installation-catalog.py`.
Configure a tag ruleset for `linuxkit-platform-v*` that restricts creation to
release operators and prohibits update, force-update, and deletion; also
reserve the exact tag name `main` (GitHub resolves a reusable workflow tag
before a same-named branch). The monotonic sequence and source-lineage checks
use those immutable tags as their high-water mark. The `platform-release`
environment must require an independent reviewer, disallow administrator
bypass, and expose the signing secret only to the protected release-tag
deployment rule. Changes to the fixed public-key repository variable must use
the same reviewed key-rotation procedure. The independent signer fails closed on a tag
outside `main`, unexpected assets, noncanonical or ambiguous manifest JSON,
release identity mismatch, archive hash/size or USTAR inventory changes, unsafe
modes, sidecar mismatches, and invalid catalog signatures.

The environment must define `LIGHTHOUSE_PLATFORM_ED25519_PRIVATE_KEY` as an
unencrypted PEM Ed25519 private key. The repository must also define
`LIGHTHOUSE_RUNTIME_ED25519_PUBLIC_KEY_BASE64` as the canonical
base64 encoding of the expected raw 32-byte public key. The job derives the raw
public key from the private key and requires an exact match before OpenSSL signs
each runtime manifest. The release job fails closed when either key setting or
either signature is absent. The same public key must be pinned in LightHouse's
`GuestRuntime.lock.json`; the PEM uploaded beside a release is diagnostic
metadata, not a trust root.

Project-original sources and runtime files are identified as
`LicenseRef-LightHouse-Original`: copyright remains with the project copyright
holders, and the inventory does not infer or grant an open-source license.
Static guest executables also retain the exact Zig 0.16.0 MIT license and musl
COPYRIGHT notice from the pinned toolchain. The release archive, license
metadata, and SPDX document carry all of these notices. See the repository
[`LICENSES/`](../LICENSES) inventory; the workflow fails closed if its coverage
or pinned notice digests are incomplete.
