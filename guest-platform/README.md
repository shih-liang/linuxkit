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

Zig cross-compiles static musl binaries for both supported guest architectures.
The host tests require only a C compiler and Python 3.

```sh
make -C guest-platform test
make -C guest-platform test-license
make -C guest-platform build
```

`.github/workflows/build-linux.yml` runs these steps for every push to
`main`. Its `linuxkit-linux` artifact is checkout-shaped: downloading it at
the repository root restores `guest-platform/build/aarch64` and
`guest-platform/build/x86_64`. FluxWindow copies those binaries together with
the adapters, service files, and catalogs from the same local checkout.

Kernel and initramfs builds remain separate in `build-kernel.yml`; they are
downloaded by FluxWindow as VM boot resources and are not embedded in the
application's guest runtime.

## GitHub releases

Tags matching `linuxkit-guest-v*` publish one archive per architecture.
A release also contains `SHA256SUMS`, its Ed25519 signature, and the matching
public key. Signing runs without repository write permission; a separate job
checks the digest and signature before publishing the GitHub Release.

The independently signed
`Resources/GuestEnvironmentCatalog.signed.json` remains the online catalog
update envelope. It is separate from build-artifact selection.

Project-original sources remain `LicenseRef-LightHouse-Original`. Static guest
executables retain the exact Zig 0.16.0 MIT license and musl COPYRIGHT notice
recorded under `LICENSES/`.
