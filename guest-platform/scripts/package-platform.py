#!/usr/bin/env python3
"""Build deterministic LightHouse guest-platform release artifacts."""

import argparse
import base64
import datetime as dt
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import tarfile
import tempfile


COMPONENT = "linuxkit-platform"
GUEST_RUNTIME_ABI = 1
MAXIMUM_RELEASE_SEQUENCE = (1 << 64) - 1
MIGRATION_REPOSITORY = "https://github.com/shih-liang/LightHouse"
MIGRATION_COMMIT = "4cb1e4083c795d714a5fb5950cba8fa6772e3235"
ARCHITECTURES = ("aarch64", "x86_64")
ADAPTER_FILES = (
    "README.md",
    "archlinux.sh",
    "catalog-public-key.txt",
    "catalog.json",
    "catalog.signed.json",
    "common.sh",
    "fedora.sh",
    "ubuntu.sh",
    "udhcpc.sh",
    "validate.py",
)
ENVIRONMENT_RESOURCE_FILES = (
    "GuestEnvironmentCatalog.json",
    "GuestEnvironmentCatalogPublicKey.txt",
)
LICENSE_FILES = (
    "README.md",
    "LightHouse-Original.txt",
    "Zig-MIT.txt",
    "musl-COPYRIGHT.txt",
    "source-inventory.json",
)
LICENSE_ARCHIVE_PREFIX = "LICENSES/linuxkit-platform/"
ORIGINAL_LICENSE = "LicenseRef-LightHouse-Original"
MUSL_LICENSE = "LicenseRef-musl-COPYRIGHT"
BINARY_LICENSE = "%s AND MIT AND %s" % (ORIGINAL_LICENSE, MUSL_LICENSE)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def canonical_json(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def atomic_write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".%s." % path.name, dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def require_regular_file(path):
    try:
        file_mode = path.lstat().st_mode
    except FileNotFoundError as error:
        raise SystemExit("missing platform input: %s" % path) from error
    if not stat.S_ISREG(file_mode):
        raise SystemExit("platform input must be a regular file: %s" % path)


def safe_relative_path(value):
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in path.parts):
        raise SystemExit("unsafe archive path: %s" % value)
    normalized = str(path)
    if normalized != value:
        raise SystemExit("archive path is not normalized: %s" % value)
    return normalized


def payload_sources(root, build_dir, architecture):
    sources = {
        "guest/bootstrap/dist/nativepipe-bootstrap-%s" % architecture: (
            build_dir / architecture / "nativepipe-bootstrap",
            0o755,
        ),
        "guest/guestd/VERSION": (root / "guestd/VERSION", 0o644),
        "guest/guestd/dist/nativepipe-guestd-%s" % architecture: (
            build_dir / architecture / "nativepipe-guestd",
            0o755,
        ),
        "guest/guestd/install.sh": (root / "guestd/install.sh", 0o755),
        "guest/guestd/nativepipe.interfaces": (root / "guestd/nativepipe.interfaces", 0o644),
        "guest/guestd/nativepipe.modules": (root / "guestd/nativepipe.modules", 0o644),
        "guest/guestd/openrc/nativepipe-guestd": (
            root / "guestd/openrc/nativepipe-guestd",
            0o755,
        ),
        "guest/guestd/systemd/nativepipe-guestd.service": (
            root / "guestd/systemd/nativepipe-guestd.service",
            0o644,
        ),
    }
    adapters = root.parent / "adapters"
    for name in ADAPTER_FILES:
        sources["InstallAdapters/" + name] = (adapters / name, 0o644)
    resources = root.parent / "Resources"
    for name in ENVIRONMENT_RESOURCE_FILES:
        sources["Resources/" + name] = (resources / name, 0o644)
    licenses = root.parent / "LICENSES"
    for name in LICENSE_FILES:
        sources[LICENSE_ARCHIVE_PREFIX + name] = (licenses / name, 0o644)
    return sources


def load_signed_environment_catalog(root):
    resources = root.parent / "Resources"
    catalog_data = (resources / "GuestEnvironmentCatalog.json").read_bytes()
    envelope_path = resources / "GuestEnvironmentCatalog.signed.json"
    key_path = resources / "GuestEnvironmentCatalogPublicKey.txt"
    require_regular_file(envelope_path)
    require_regular_file(key_path)
    try:
        envelope = json.loads(envelope_path.read_text(encoding="utf-8"))
        payload = base64.b64decode(envelope["payload"], validate=True)
        signature = base64.b64decode(envelope["signature"], validate=True)
        public_key = base64.b64decode(key_path.read_text(encoding="ascii").strip(), validate=True)
    except (KeyError, ValueError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit("invalid signed environment catalog: %s" % error) from error
    if payload != catalog_data:
        raise SystemExit("signed environment catalog payload does not match bundled catalog")
    if not isinstance(envelope.get("keyID"), str) or not envelope["keyID"]:
        raise SystemExit("signed environment catalog has no keyID")
    if len(signature) != 64 or len(public_key) != 32:
        raise SystemExit("signed environment catalog has an invalid Ed25519 key/signature size")
    return envelope_path.read_bytes()


def load_payload(root, build_dir, architecture):
    payload = []
    for archive_path, (source_path, mode) in sorted(
        payload_sources(root, build_dir, architecture).items()
    ):
        safe_relative_path(archive_path)
        require_regular_file(source_path)
        data = source_path.read_bytes()
        payload.append(
            {
                "path": archive_path,
                "data": data,
                "size": len(data),
                "sha256": sha256_bytes(data),
                "mode": mode,
            }
        )
    return payload


def verify_static_elf(data, architecture, path, version=None):
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise SystemExit("platform binary is not ELF: %s" % path)
    if data[4] != 2 or data[5] != 1:
        raise SystemExit("platform binary must be little-endian ELF64: %s" % path)
    expected_machine = {"aarch64": 183, "x86_64": 62}[architecture]
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != expected_machine:
        raise SystemExit("platform binary has the wrong architecture: %s" % path)
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size = struct.unpack_from("<H", data, 54)[0]
    entry_count = struct.unpack_from("<H", data, 56)[0]
    if entry_size < 4 or program_offset + entry_size * entry_count > len(data):
        raise SystemExit("platform binary has an invalid program-header table: %s" % path)
    for index in range(entry_count):
        program_type = struct.unpack_from("<I", data, program_offset + index * entry_size)[0]
        if program_type == 3:
            raise SystemExit("platform binary is dynamically linked: %s" % path)
    if version is not None and ("NPGV:" + version).encode("ascii") not in data:
        raise SystemExit("guestd binary does not contain the VERSION stamp: %s" % path)


def payload_directories(payload):
    directories = set()
    for item in payload:
        parent = PurePosixPath(item["path"]).parent
        while str(parent) != ".":
            directories.add(str(parent))
            parent = parent.parent
    return sorted(directories)


def build_ustar(payload, source_date_epoch):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for directory in payload_directories(payload):
            info = tarfile.TarInfo(directory + "/")
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mtime = source_date_epoch
            archive.addfile(info)
        for item in payload:
            info = tarfile.TarInfo(item["path"])
            info.type = tarfile.REGTYPE
            info.mode = item["mode"]
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mtime = source_date_epoch
            info.size = item["size"]
            archive.addfile(info, io.BytesIO(item["data"]))
    return output.getvalue()


def deterministic_gzip(data, source_date_epoch):
    output = io.BytesIO()
    with gzip.GzipFile(
        filename="", mode="wb", fileobj=output, compresslevel=9, mtime=source_date_epoch
    ) as stream:
        stream.write(data)
    return output.getvalue()


def source_date(source_date_epoch):
    return dt.datetime.fromtimestamp(source_date_epoch, tz=dt.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )


def spdx_id(path):
    digest = hashlib.sha256(path.encode("utf-8")).hexdigest()[:16]
    return "SPDXRef-File-%s" % digest


def runtime_manifest(
    version,
    architecture,
    source_repository,
    source_commit,
    release_tag,
    release_sequence,
    archive_name,
    archive_data,
    payload,
):
    return {
        "schemaVersion": 1,
        "component": COMPONENT,
        "version": version,
        "sourceRepository": source_repository,
        "sourceCommit": source_commit,
        "releaseTag": release_tag,
        "releaseSequence": release_sequence,
        "architecture": architecture,
        "guestRuntimeABI": GUEST_RUNTIME_ABI,
        "archive": {
            "name": archive_name,
            "size": len(archive_data),
            "sha256": sha256_bytes(archive_data),
        },
        "files": [
            {
                "path": item["path"],
                "size": item["size"],
                "sha256": item["sha256"],
                "mode": item["mode"],
            }
            for item in payload
        ],
    }


def source_metadata(
    version,
    architecture,
    source_repository,
    source_commit,
    release_tag,
    release_sequence,
    source_date_epoch,
):
    return {
        "schemaVersion": 1,
        "component": COMPONENT,
        "version": version,
        "architecture": architecture,
        "releaseTag": release_tag,
        "releaseSequence": release_sequence,
        "sourceRepository": source_repository,
        "sourceCommit": source_commit,
        "sourceDateEpoch": source_date_epoch,
        "sourceDate": source_date(source_date_epoch),
        "migratedFrom": {
            "repository": MIGRATION_REPOSITORY,
            "commit": MIGRATION_COMMIT,
            "paths": [
                "guest/common",
                "guest/bootstrap",
                "guest/guestd",
                "Resources/GuestEnvironmentCatalog.json",
                "Resources/GuestEnvironmentCatalog.signed.json",
                "Resources/GuestEnvironmentCatalogPublicKey.txt",
            ],
        },
        "buildToolchain": {
            "compiler": "Zig",
            "version": "0.16.0",
            "targetLibc": "musl",
            "notices": [
                LICENSE_ARCHIVE_PREFIX + "Zig-MIT.txt",
                LICENSE_ARCHIVE_PREFIX + "musl-COPYRIGHT.txt",
            ],
        },
    }


def file_license_identifiers(path):
    if path.startswith("guest/bootstrap/dist/") or path.startswith(
        "guest/guestd/dist/"
    ):
        return [ORIGINAL_LICENSE, "MIT", MUSL_LICENSE]
    if path == LICENSE_ARCHIVE_PREFIX + "Zig-MIT.txt":
        return ["MIT"]
    if path == LICENSE_ARCHIVE_PREFIX + "musl-COPYRIGHT.txt":
        return [MUSL_LICENSE]
    return [ORIGINAL_LICENSE]


def file_license_expression(path):
    return " AND ".join(file_license_identifiers(path))


def file_copyright_text(path):
    if path.startswith("guest/bootstrap/dist/") or path.startswith(
        "guest/guestd/dist/"
    ):
        return (
            "LightHouse project copyright holders; Zig contributors; "
            "musl authors and contributors"
        )
    if path == LICENSE_ARCHIVE_PREFIX + "Zig-MIT.txt":
        return "Copyright (c) Zig contributors"
    if path == LICENSE_ARCHIVE_PREFIX + "musl-COPYRIGHT.txt":
        return "Copyright 2005-2020 Rich Felker, et al.; see the retained COPYRIGHT text"
    return "Copyright remains with the LightHouse project copyright holders"


def payload_text(payload, path):
    for item in payload:
        if item["path"] == path:
            return item["data"].decode("utf-8")
    raise SystemExit("license payload is absent: %s" % path)


def license_metadata(version, architecture, release_tag, release_sequence, payload):
    return {
        "schemaVersion": 1,
        "component": COMPONENT,
        "version": version,
        "architecture": architecture,
        "releaseTag": release_tag,
        "releaseSequence": release_sequence,
        "publicationStatus": "READY",
        "declaredLicense": BINARY_LICENSE,
        "concludedLicense": BINARY_LICENSE,
        "licenseFiles": [LICENSE_ARCHIVE_PREFIX + name for name in LICENSE_FILES],
        "notice": (
            "Project-original material remains all rights reserved under "
            "LicenseRef-LightHouse-Original; this does not infer or grant an "
            "open-source license. Static guest executables retain the exact Zig "
            "0.16.0 MIT and musl COPYRIGHT notices shipped in the archive."
        ),
        "files": [
            {
                "path": item["path"],
                "licenseConcluded": file_license_expression(item["path"]),
            }
            for item in payload
        ],
    }


def spdx_metadata(
    version,
    architecture,
    source_repository,
    source_commit,
    release_tag,
    source_date_epoch,
    archive_name,
    archive_data,
    payload,
):
    repository_url = "https://github.com/%s" % source_repository
    package_id = "SPDXRef-Package-LightHouse-Platform"
    files = []
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": package_id,
        }
    ]
    for item in payload:
        file_id = spdx_id(item["path"])
        files.append(
            {
                "SPDXID": file_id,
                "fileName": "./" + item["path"],
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": item["sha256"]}
                ],
                "licenseConcluded": file_license_expression(item["path"]),
                "licenseInfoInFiles": file_license_identifiers(item["path"]),
                "copyrightText": file_copyright_text(item["path"]),
            }
        )
        relationships.append(
            {
                "spdxElementId": package_id,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_id,
            }
        )
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "%s-%s-%s" % (COMPONENT, version, architecture),
        "documentNamespace": "%s/releases/download/%s/spdx/%s/%s"
        % (repository_url, release_tag, source_commit, architecture),
        "creationInfo": {
            "created": source_date(source_date_epoch),
            "creators": ["Tool: linuxkit-platform-packager-1"],
        },
        "packages": [
            {
                "SPDXID": package_id,
                "name": COMPONENT,
                "versionInfo": version,
                "downloadLocation": "%s/releases/download/%s/%s"
                % (repository_url, release_tag, archive_name),
                "filesAnalyzed": True,
                "licenseConcluded": BINARY_LICENSE,
                "licenseDeclared": BINARY_LICENSE,
                "copyrightText": (
                    "LightHouse project copyright holders; Zig contributors; "
                    "musl authors and contributors"
                ),
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": sha256_bytes(archive_data)}
                ],
            }
        ],
        "files": files,
        "relationships": relationships,
        "hasExtractedLicensingInfos": [
            {
                "licenseId": ORIGINAL_LICENSE,
                "name": "LightHouse project-original material",
                "extractedText": payload_text(
                    payload, LICENSE_ARCHIVE_PREFIX + "LightHouse-Original.txt"
                ),
            },
            {
                "licenseId": MUSL_LICENSE,
                "name": "musl COPYRIGHT terms bundled with Zig 0.16.0",
                "extractedText": payload_text(
                    payload, LICENSE_ARCHIVE_PREFIX + "musl-COPYRIGHT.txt"
                ),
            },
        ],
    }


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture", required=True, choices=ARCHITECTURES)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--release-sequence", required=True, type=int)
    parser.add_argument("--source-repository", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-date-epoch", required=True, type=int)
    return parser.parse_args()


def main():
    args = parse_arguments()
    root = Path(__file__).resolve().parent.parent
    version = (root / "guestd/VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise SystemExit("VERSION must be a three-part numeric version")
    if not 1 <= args.release_sequence <= MAXIMUM_RELEASE_SEQUENCE:
        raise SystemExit("release sequence must be in 1...UInt64.max")
    expected_tag = "linuxkit-platform-v%s-%d" % (version, args.release_sequence)
    if args.release_tag != expected_tag:
        raise SystemExit("release tag must be exactly %s" % expected_tag)
    if not re.fullmatch(r"[0-9a-f]{40,64}", args.source_commit):
        raise SystemExit("source commit must be a lowercase hexadecimal Git commit")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.source_repository):
        raise SystemExit("source repository must be an owner/repository slug")
    if args.source_date_epoch < 0:
        raise SystemExit("source date epoch must not be negative")

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    payload = load_payload(root, build_dir, args.architecture)
    for item in payload:
        if item["path"].endswith("nativepipe-bootstrap-" + args.architecture):
            verify_static_elf(item["data"], args.architecture, item["path"])
        if item["path"].endswith("nativepipe-guestd-" + args.architecture):
            verify_static_elf(item["data"], args.architecture, item["path"], version)
    archive_data = build_ustar(payload, args.source_date_epoch)
    archive_name = "%s-%s.tar" % (COMPONENT, args.architecture)
    gzip_name = archive_name + ".gz"
    manifest_name = "%s-%s.runtime-manifest.json" % (
        COMPONENT, args.architecture,
    )
    prefix = "%s-%s" % (COMPONENT, args.architecture)

    manifest = runtime_manifest(
        version,
        args.architecture,
        args.source_repository,
        args.source_commit,
        args.release_tag,
        args.release_sequence,
        archive_name,
        archive_data,
        payload,
    )
    source = source_metadata(
        version,
        args.architecture,
        args.source_repository,
        args.source_commit,
        args.release_tag,
        args.release_sequence,
        args.source_date_epoch,
    )
    licenses = license_metadata(
        version, args.architecture, args.release_tag, args.release_sequence, payload
    )
    spdx = spdx_metadata(
        version,
        args.architecture,
        args.source_repository,
        args.source_commit,
        args.release_tag,
        args.source_date_epoch,
        archive_name,
        archive_data,
        payload,
    )

    atomic_write(output_dir / archive_name, archive_data)
    atomic_write(
        output_dir / gzip_name,
        deterministic_gzip(archive_data, args.source_date_epoch),
    )
    atomic_write(output_dir / manifest_name, canonical_json(manifest))
    atomic_write(output_dir / (prefix + ".source.json"), canonical_json(source))
    atomic_write(output_dir / (prefix + ".licenses.json"), canonical_json(licenses))
    atomic_write(output_dir / (prefix + ".spdx.json"), canonical_json(spdx))
    atomic_write(
        output_dir / "GuestEnvironmentCatalog.signed.json",
        load_signed_environment_catalog(root),
    )

    print(output_dir / archive_name)
    print(output_dir / manifest_name)


if __name__ == "__main__":
    main()
