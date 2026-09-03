#!/usr/bin/env python3
"""Mechanical reproducibility and safe-archive tests for platform releases."""

import argparse
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fail(message):
    raise AssertionError(message)


def run_packager(
    root,
    architecture,
    output,
    source_commit,
    source_date_epoch,
    release_sequence=1,
    check=True,
):
    version = (root / "guestd/VERSION").read_text(encoding="utf-8").strip()
    tag = "linuxkit-platform-v%s-%d" % (version, release_sequence)
    return subprocess.run(
        [
            sys.executable,
            str(root / "scripts/package-platform.py"),
            "--architecture",
            architecture,
            "--build-dir",
            str(root / "build"),
            "--output-dir",
            str(output),
            "--release-tag",
            tag,
            "--release-sequence",
            str(release_sequence),
            "--source-repository",
            "shih-liang/linuxkit",
            "--source-commit",
            source_commit,
            "--source-date-epoch",
            str(source_date_epoch),
        ],
        check=check,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def run_release_verifier(
    root, output, source_commit, source_date_epoch, check=True
):
    version = (root / "guestd/VERSION").read_text(encoding="utf-8").strip()
    openssl = os.environ.get("OPENSSL", "")
    homebrew_openssl = Path("/opt/homebrew/opt/openssl@3/bin/openssl")
    if not openssl:
        openssl = str(homebrew_openssl) if homebrew_openssl.is_file() else "openssl"
    return subprocess.run(
        [
            sys.executable,
            str(root / "scripts/verify-platform-release.py"),
            "--asset-dir",
            str(output),
            "--expected-version",
            version,
            "--expected-tag",
            "linuxkit-platform-v%s-1" % version,
            "--expected-sequence",
            "1",
            "--expected-repository",
            "shih-liang/linuxkit",
            "--expected-commit",
            source_commit,
            "--expected-source-date-epoch",
            str(source_date_epoch),
            "--openssl",
            openssl,
        ],
        check=check,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def output_bytes(directory):
    return {
        path.name: path.read_bytes()
        for path in sorted(directory.iterdir())
        if path.is_file()
    }


def verify_archive(root, architecture, output, source_commit, source_date_epoch):
    version = (root / "guestd/VERSION").read_text(encoding="utf-8").strip()
    prefix = "linuxkit-platform-%s" % architecture
    archive_path = output / (prefix + ".tar")
    gzip_path = output / (prefix + ".tar.gz")
    manifest_path = output / (prefix + ".runtime-manifest.json")
    archive_data = archive_path.read_bytes()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    if archive_data[257:263] != b"ustar\x00":
        fail("archive does not have a ustar header")
    if gzip.decompress(gzip_path.read_bytes()) != archive_data:
        fail("gzip convenience asset does not contain the authoritative tar")
    if manifest["schemaVersion"] != 1 or manifest["component"] != "linuxkit-platform":
        fail("runtime manifest identity is wrong")
    expected_manifest_fields = {
        "schemaVersion",
        "component",
        "version",
        "sourceRepository",
        "sourceCommit",
        "releaseTag",
        "releaseSequence",
        "architecture",
        "guestRuntimeABI",
        "archive",
        "files",
    }
    if set(manifest) != expected_manifest_fields:
        fail("runtime manifest must keep the frozen 11-field schema")
    if manifest["guestRuntimeABI"] != 1:
        fail("runtime ABI is wrong")
    if manifest["architecture"] != architecture:
        fail("runtime manifest architecture is wrong")
    if manifest["releaseTag"] != "linuxkit-platform-v%s-1" % version:
        fail("runtime manifest tag is wrong")
    if manifest["releaseSequence"] != 1:
        fail("runtime manifest sequence is wrong")
    if not isinstance(manifest["releaseSequence"], int) or manifest["releaseSequence"] <= 0:
        fail("runtime set generation must be a positive integer")
    if manifest["sourceCommit"] != source_commit:
        fail("runtime manifest source commit is wrong")
    if manifest["sourceRepository"] != "shih-liang/linuxkit":
        fail("runtime manifest source repository is wrong")
    if manifest["archive"] != {
        "name": archive_path.name,
        "size": len(archive_data),
        "sha256": digest(archive_data),
    }:
        fail("runtime manifest archive identity is wrong")
    if "signature" in manifest:
        fail("runtime manifest contains a redundant signature descriptor")

    expected_files = {item["path"]: item for item in manifest["files"]}
    if len(expected_files) != len(manifest["files"]):
        fail("runtime manifest has duplicate paths")
    if "InstallAdapters/catalog.json" not in expected_files:
        fail("installation catalog is absent from the verified runtime set")
    if "InstallAdapters/catalog.signed.json" not in expected_files:
        fail("signed installation catalog is absent from the verified runtime set")
    if "Resources/GuestEnvironmentCatalog.json" not in expected_files:
        fail("guest environment catalog is absent from the verified runtime set")
    if "Resources/GuestEnvironmentCatalogPublicKey.txt" not in expected_files:
        fail("guest environment catalog trust key is absent from the verified runtime set")
    required_license_files = {
        "LICENSES/linuxkit-platform/README.md",
        "LICENSES/linuxkit-platform/LightHouse-Original.txt",
        "LICENSES/linuxkit-platform/Zig-MIT.txt",
        "LICENSES/linuxkit-platform/musl-COPYRIGHT.txt",
        "LICENSES/linuxkit-platform/source-inventory.json",
    }
    if not required_license_files <= expected_files.keys():
        fail("release notices are absent from the verified runtime set")
    if "LICENSES/source-inventory.json" in expected_files:
        fail("platform inventory is not isolated under its component namespace")
    required_runtime_files = {
        "guest/bootstrap/dist/nativepipe-bootstrap-%s" % architecture,
        "guest/guestd/dist/nativepipe-guestd-%s" % architecture,
        "guest/guestd/VERSION",
        "guest/guestd/install.sh",
        "guest/guestd/systemd/nativepipe-guestd.service",
        "guest/guestd/openrc/nativepipe-guestd",
        "guest/guestd/nativepipe.interfaces",
        "guest/guestd/nativepipe.modules",
    }
    if not required_runtime_files <= expected_files.keys():
        fail("runtime archive is missing a guest platform installation input")
    forbidden_suffixes = (".c", ".h")
    if any(path.endswith(forbidden_suffixes) or path.endswith("/Makefile")
           or "/tests/" in path for path in expected_files):
        fail("runtime archive contains development source or tests")

    actual_files = {}
    with tarfile.open(archive_path, mode="r:") as archive:
        members = archive.getmembers()
        member_names = [member.name for member in members]
        if len(member_names) != len(set(member_names)):
            fail("archive contains duplicate paths")
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or "." in path.parts:
                fail("archive contains an unsafe path: %s" % member.name)
            if not (member.isdir() or member.isreg()):
                fail("archive contains a non-file/non-directory entry: %s" % member.name)
            if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
                fail("archive ownership is not normalized: %s" % member.name)
            if member.mtime != source_date_epoch:
                fail("archive timestamp is not normalized: %s" % member.name)
            if member.isdir():
                if member.mode != 0o755:
                    fail("archive directory mode is wrong: %s" % member.name)
                continue
            stream = archive.extractfile(member)
            if stream is None:
                fail("regular file cannot be read: %s" % member.name)
            data = stream.read()
            actual_files[member.name] = {
                "path": member.name,
                "size": len(data),
                "sha256": digest(data),
                "mode": member.mode,
            }
    if actual_files != expected_files:
        fail("runtime manifest file inventory does not match the archive")

    signed_catalog = json.loads(
        (output / "GuestEnvironmentCatalog.signed.json").read_text(encoding="utf-8")
    )
    if base64.b64decode(signed_catalog["payload"], validate=True) != (
        root.parent / "Resources/GuestEnvironmentCatalog.json"
    ).read_bytes():
        fail("published signed environment catalog does not match bundled catalog")
    if len(base64.b64decode(signed_catalog["signature"], validate=True)) != 64:
        fail("published environment catalog signature is not Ed25519-sized")
    if len(base64.b64decode(
        (root.parent / "Resources/GuestEnvironmentCatalogPublicKey.txt")
        .read_text(encoding="ascii").strip(), validate=True
    )) != 32:
        fail("bundled environment catalog public key is not Ed25519-sized")

    for suffix in ("source.json", "licenses.json", "spdx.json"):
        metadata = json.loads((output / (prefix + "." + suffix)).read_text(encoding="utf-8"))
        if metadata.get("component") != "linuxkit-platform" and suffix != "spdx.json":
            fail("metadata component is wrong: %s" % suffix)
    spdx = json.loads((output / (prefix + ".spdx.json")).read_text(encoding="utf-8"))
    if spdx.get("spdxVersion") != "SPDX-2.3":
        fail("SPDX metadata version is wrong")
    licenses = json.loads(
        (output / (prefix + ".licenses.json")).read_text(encoding="utf-8")
    )
    if licenses.get("publicationStatus") != "READY":
        fail("reviewed license inventory is not publication-ready")
    expected_binary_license = (
        "LicenseRef-LightHouse-Original AND MIT AND LicenseRef-musl-COPYRIGHT"
    )
    if licenses.get("declaredLicense") != expected_binary_license:
        fail("release license expression does not cover original/Zig/musl terms")
    if "NOASSERTION" in json.dumps(licenses, sort_keys=True):
        fail("license metadata contains unresolved assertions")
    if "NOASSERTION" in json.dumps(spdx, sort_keys=True):
        fail("SPDX metadata contains unresolved assertions")
    extracted = {
        item.get("licenseId")
        for item in spdx.get("hasExtractedLicensingInfos", [])
    }
    if extracted != {
        "LicenseRef-LightHouse-Original",
        "LicenseRef-musl-COPYRIGHT",
    }:
        fail("SPDX metadata omits custom license texts")


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-date-epoch", required=True, type=int)
    return parser.parse_args()


def main():
    args = parse_arguments()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="linuxkit-platform-test-") as temporary:
        temporary = Path(temporary)
        oversized = run_packager(
            root,
            "aarch64",
            temporary / "oversized",
            args.source_commit,
            args.source_date_epoch,
            release_sequence=(1 << 64),
            check=False,
        )
        if oversized.returncode == 0:
            fail("packager accepts a release sequence greater than UInt64.max")
        for architecture in ("aarch64", "x86_64"):
            first = temporary / (architecture + "-first")
            second = temporary / (architecture + "-second")
            run_packager(root, architecture, first, args.source_commit, args.source_date_epoch)
            run_packager(root, architecture, second, args.source_commit, args.source_date_epoch)
            if output_bytes(first) != output_bytes(second):
                fail("packaging is not reproducible for %s" % architecture)
            verify_archive(
                root,
                architecture,
                first,
                args.source_commit,
                args.source_date_epoch,
            )
        release = temporary / "release"
        release.mkdir()
        for architecture in ("aarch64", "x86_64"):
            for path in (temporary / (architecture + "-first")).iterdir():
                if path.name == "GuestEnvironmentCatalog.signed.json" and (
                    release / path.name
                ).exists():
                    continue
                shutil.copy2(path, release / path.name)
        run_release_verifier(
            root, release, args.source_commit, args.source_date_epoch
        )

        tampered = temporary / "tampered"
        shutil.copytree(release, tampered)
        archive = tampered / "linuxkit-platform-aarch64.tar"
        data = bytearray(archive.read_bytes())
        data[1024] ^= 1
        archive.write_bytes(data)
        if run_release_verifier(
            root,
            tampered,
            args.source_commit,
            args.source_date_epoch,
            check=False,
        ).returncode == 0:
            fail("release verifier accepts a tampered runtime archive")

        tampered = temporary / "tampered-sidecar"
        shutil.copytree(release, tampered)
        source_path = tampered / "linuxkit-platform-x86_64.source.json"
        source = json.loads(source_path.read_text(encoding="utf-8"))
        source["releaseSequence"] = 2
        source_path.write_text(
            json.dumps(source, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        if run_release_verifier(
            root,
            tampered,
            args.source_commit,
            args.source_date_epoch,
            check=False,
        ).returncode == 0:
            fail("release verifier accepts mismatched source metadata")
    print("guest-platform packaging tests: ok")


if __name__ == "__main__":
    main()
