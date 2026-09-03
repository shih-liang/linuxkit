#!/usr/bin/env python3
"""Offline verifier for an unsigned LightHouse guest-platform release candidate."""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile
from typing import Any


COMPONENT = "linuxkit-platform"
GUEST_RUNTIME_ABI = 1
SCHEMA_VERSION = 1
ARCHITECTURES = ("aarch64", "x86_64")
MAXIMUM_RELEASE_SEQUENCE = (1 << 64) - 1
MAXIMUM_ARCHIVE_BYTES = 512 * 1024 * 1024
MAXIMUM_MANIFEST_BYTES = 2 * 1024 * 1024
MAXIMUM_FILES = 256
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")

MANIFEST_FIELDS = {
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
ARCHIVE_FIELDS = {"name", "size", "sha256"}
FILE_FIELDS = {"path", "size", "sha256", "mode"}
SOURCE_FIELDS = {
    "schemaVersion",
    "component",
    "version",
    "architecture",
    "releaseTag",
    "releaseSequence",
    "sourceRepository",
    "sourceCommit",
    "sourceDateEpoch",
    "sourceDate",
    "migratedFrom",
    "buildToolchain",
}
LICENSE_FIELDS = {
    "schemaVersion",
    "component",
    "version",
    "architecture",
    "releaseTag",
    "releaseSequence",
    "publicationStatus",
    "declaredLicense",
    "concludedLicense",
    "licenseFiles",
    "notice",
    "files",
}
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
RESOURCE_FILES = (
    "GuestEnvironmentCatalog.json",
    "GuestEnvironmentCatalogPublicKey.txt",
)
RELEASE_LICENSE_FILES = (
    "README.md",
    "LightHouse-Original.txt",
    "Zig-MIT.txt",
    "musl-COPYRIGHT.txt",
    "source-inventory.json",
)
LICENSE_ARCHIVE_PREFIX = "LICENSES/linuxkit-platform/"
LICENSE_EXPRESSION = (
    "LicenseRef-LightHouse-Original AND MIT AND LicenseRef-musl-COPYRIGHT"
)


class VerificationError(RuntimeError):
    """A candidate failed a release policy check."""


def canonical_json(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def read_json(path: Path, label: str, *, canonical: bool = False) -> Any:
    data = regular_file_bytes(path, label)
    try:
        value = json.loads(
            data.decode("utf-8"), object_pairs_hook=reject_duplicate_keys
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"invalid {label}: {error}") from error
    if canonical and data != canonical_json(value):
        raise VerificationError(f"{label} is not canonical JSON")
    return value


def regular_file_bytes(path: Path, label: str, maximum: int | None = None) -> bytes:
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise VerificationError(f"missing {label}: {path.name}") from error
    if path.is_symlink() or not path.is_file():
        raise VerificationError(f"{label} is not a regular file: {path.name}")
    if maximum is not None and metadata.st_size > maximum:
        raise VerificationError(f"{label} exceeds its size limit")
    try:
        return path.read_bytes()
    except OSError as error:
        raise VerificationError(f"cannot read {label}: {error}") from error


def exact_int(value: Any, label: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise VerificationError(f"{label} must be an integer >= {minimum}")
    return value


def uint64(value: Any, label: str) -> int:
    result = exact_int(value, label, minimum=1)
    if result > MAXIMUM_RELEASE_SEQUENCE:
        raise VerificationError(f"{label} exceeds UInt64")
    return result


def nonempty_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise VerificationError(f"{label} must be a non-empty string")
    return value


def safe_path(value: Any, label: str) -> str:
    value = nonempty_string(value, label)
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or not path.parts
        or any(part in ("", ".", "..") for part in path.parts)
        or path.as_posix() != value
    ):
        raise VerificationError(f"{label} is unsafe or non-normalized: {value}")
    return value


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def required_files(architecture: str) -> dict[str, int]:
    files = {
        "guest/bootstrap/dist/nativepipe-bootstrap-%s" % architecture: 0o755,
        "guest/guestd/VERSION": 0o644,
        "guest/guestd/dist/nativepipe-guestd-%s" % architecture: 0o755,
        "guest/guestd/install.sh": 0o755,
        "guest/guestd/nativepipe.interfaces": 0o644,
        "guest/guestd/nativepipe.modules": 0o644,
        "guest/guestd/openrc/nativepipe-guestd": 0o755,
        "guest/guestd/systemd/nativepipe-guestd.service": 0o644,
    }
    files.update({"InstallAdapters/" + name: 0o644 for name in ADAPTER_FILES})
    files.update({"Resources/" + name: 0o644 for name in RESOURCE_FILES})
    files.update({LICENSE_ARCHIVE_PREFIX + name: 0o644 for name in RELEASE_LICENSE_FILES})
    return files


def required_directories(paths: set[str]) -> set[str]:
    result: set[str] = set()
    for name in paths:
        parent = PurePosixPath(name).parent
        while parent.as_posix() != ".":
            result.add(parent.as_posix())
            parent = parent.parent
    return result


def verify_static_elf(data: bytes, architecture: str, label: str) -> None:
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise VerificationError(f"{label} is not little-endian ELF64")
    machine = int.from_bytes(data[18:20], "little")
    if machine != {"aarch64": 183, "x86_64": 62}[architecture]:
        raise VerificationError(f"{label} has the wrong ELF architecture")
    program_offset = int.from_bytes(data[32:40], "little")
    entry_size = int.from_bytes(data[54:56], "little")
    entry_count = int.from_bytes(data[56:58], "little")
    if entry_size < 4 or program_offset + entry_size * entry_count > len(data):
        raise VerificationError(f"{label} has an invalid program-header table")
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        if int.from_bytes(data[offset : offset + 4], "little") == 3:
            raise VerificationError(f"{label} is dynamically linked")


def verify_raw_ustar(data: bytes) -> None:
    if not data or len(data) % 512:
        raise VerificationError("archive is not a non-empty aligned tar stream")
    offset = 0
    zero_blocks = 0
    while offset < len(data):
        header = data[offset : offset + 512]
        if header == bytes(512):
            zero_blocks += 1
            offset += 512
            if zero_blocks >= 2:
                break
            continue
        if zero_blocks:
            raise VerificationError("archive contains data after an end marker")
        if header[257:263] != b"ustar\x00" or header[263:265] != b"00":
            raise VerificationError("archive contains a non-USTAR header")
        if header[156:157] not in (b"\x00", b"0", b"5"):
            raise VerificationError("archive contains an unsupported entry type")
        raw_size = header[124:136].rstrip(b"\x00 ").lstrip(b" ") or b"0"
        try:
            size = int(raw_size, 8)
        except ValueError as error:
            raise VerificationError("archive contains an invalid size field") from error
        data_offset = offset + 512
        next_offset = data_offset + ((size + 511) // 512) * 512
        if next_offset > len(data):
            raise VerificationError("archive entry extends beyond the stream")
        if any(data[data_offset + size : next_offset]):
            raise VerificationError("archive contains non-zero file padding")
        offset = next_offset
    if zero_blocks < 2 or any(data[offset:]):
        raise VerificationError("archive lacks two zero end blocks")


def verify_archive(
    archive_path: Path,
    expected_files: dict[str, dict[str, Any]],
    source_date_epoch: int,
    architecture: str,
) -> dict[str, bytes]:
    archive_data = regular_file_bytes(
        archive_path, "runtime archive", maximum=MAXIMUM_ARCHIVE_BYTES
    )
    verify_raw_ustar(archive_data)
    observed_files: dict[str, dict[str, Any]] = {}
    contents: dict[str, bytes] = {}
    observed_directories: set[str] = set()
    member_names: list[str] = []
    try:
        with tarfile.open(fileobj=io.BytesIO(archive_data), mode="r:") as archive:
            for member in archive.getmembers():
                name = safe_path(member.name.rstrip("/") if member.isdir() else member.name,
                                 "archive member")
                if name in member_names:
                    raise VerificationError(f"duplicate archive member: {name}")
                member_names.append(name)
                if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
                    raise VerificationError(f"non-canonical ownership: {name}")
                if member.mtime != source_date_epoch:
                    raise VerificationError(f"non-canonical timestamp: {name}")
                if member.isdir():
                    if member.mode != 0o755 or member.size != 0:
                        raise VerificationError(f"non-canonical directory: {name}")
                    observed_directories.add(name)
                    continue
                if not member.isreg():
                    raise VerificationError(f"non-regular archive entry: {name}")
                stream = archive.extractfile(member)
                if stream is None:
                    raise VerificationError(f"cannot read archive member: {name}")
                data = stream.read()
                contents[name] = data
                observed_files[name] = {
                    "path": name,
                    "size": len(data),
                    "sha256": sha256_bytes(data),
                    "mode": member.mode,
                }
    except (OSError, tarfile.TarError) as error:
        raise VerificationError(f"cannot inspect USTAR archive: {error}") from error
    expected_directories = required_directories(set(expected_files))
    expected_order = sorted(expected_directories) + sorted(expected_files)
    if member_names != expected_order:
        raise VerificationError("archive member ordering or directory inventory is invalid")
    if observed_directories != expected_directories:
        raise VerificationError("archive directory inventory is invalid")
    if observed_files != expected_files:
        raise VerificationError("archive files do not match the signed manifest inventory")
    for program in (
        "guest/bootstrap/dist/nativepipe-bootstrap-%s" % architecture,
        "guest/guestd/dist/nativepipe-guestd-%s" % architecture,
    ):
        verify_static_elf(contents[program], architecture, program)
    return contents


def decode_canonical_base64(value: Any, label: str) -> bytes:
    if not isinstance(value, str) or any(character.isspace() for character in value):
        raise VerificationError(f"{label} is not canonical base64")
    try:
        decoded = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError) as error:
        raise VerificationError(f"{label} is not valid base64") from error
    if base64.b64encode(decoded).decode("ascii") != value:
        raise VerificationError(f"{label} is not canonical base64")
    return decoded


def verify_catalog_envelope(
    payload: bytes,
    public_key_text: bytes,
    envelope_data: bytes,
    label: str,
    openssl: str,
) -> None:
    try:
        public_value = public_key_text.decode("ascii").strip()
    except UnicodeError as error:
        raise VerificationError(f"{label} public key is not ASCII") from error
    public_key = decode_canonical_base64(public_value, f"{label} public key")
    try:
        envelope = json.loads(
            envelope_data.decode("utf-8"), object_pairs_hook=reject_duplicate_keys
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise VerificationError(f"invalid {label} envelope: {error}") from error
    if not isinstance(envelope, dict) or set(envelope) != {"keyID", "payload", "signature"}:
        raise VerificationError(f"{label} envelope has an invalid schema")
    nonempty_string(envelope.get("keyID"), f"{label}.keyID")
    signed_payload = decode_canonical_base64(envelope.get("payload"), f"{label} payload")
    signature = decode_canonical_base64(envelope.get("signature"), f"{label} signature")
    if signed_payload != payload:
        raise VerificationError(f"{label} signed payload does not match the archive")
    if len(public_key) != 32 or len(signature) != 64:
        raise VerificationError(f"{label} is not an Ed25519 key/signature pair")
    with tempfile.TemporaryDirectory(prefix="linuxkit-platform-catalog-") as temporary:
        directory = Path(temporary)
        (directory / "public.der").write_bytes(ED25519_SPKI_PREFIX + public_key)
        (directory / "payload").write_bytes(payload)
        (directory / "signature").write_bytes(signature)
        try:
            result = subprocess.run(
                [
                    openssl,
                    "pkeyutl",
                    "-verify",
                    "-rawin",
                    "-pubin",
                    "-keyform",
                    "DER",
                    "-inkey",
                    str(directory / "public.der"),
                    "-in",
                    str(directory / "payload"),
                    "-sigfile",
                    str(directory / "signature"),
                ],
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError as error:
            raise VerificationError(f"cannot execute OpenSSL for {label}: {error}") from error
    if result.returncode != 0:
        raise VerificationError(f"{label} Ed25519 signature is invalid")


def verify_source_sidecar(
    path: Path,
    identity: dict[str, Any],
    expected_source_date_epoch: int,
) -> int:
    value = read_json(path, "source metadata", canonical=True)
    if not isinstance(value, dict) or set(value) != SOURCE_FIELDS:
        raise VerificationError("source metadata field set is invalid")
    for key in (
        "schemaVersion",
        "component",
        "version",
        "architecture",
        "releaseTag",
        "releaseSequence",
        "sourceRepository",
        "sourceCommit",
    ):
        if value.get(key) != identity[key] or type(value.get(key)) is not type(identity[key]):
            raise VerificationError(f"source metadata {key} does not match the manifest")
    epoch = exact_int(value.get("sourceDateEpoch"), "sourceDateEpoch")
    if epoch != expected_source_date_epoch:
        raise VerificationError("sourceDateEpoch does not match the tagged commit")
    expected_date = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc).isoformat().replace(
        "+00:00", "Z"
    )
    if value.get("sourceDate") != expected_date:
        raise VerificationError("sourceDate does not match sourceDateEpoch")
    toolchain = value.get("buildToolchain")
    if not isinstance(toolchain, dict) or toolchain.get("compiler") != "Zig" or toolchain.get("version") != "0.16.0" or toolchain.get("targetLibc") != "musl":
        raise VerificationError("source metadata toolchain identity is invalid")
    if not isinstance(value.get("migratedFrom"), dict):
        raise VerificationError("source migration metadata is invalid")
    return epoch


def verify_license_sidecar(
    path: Path, identity: dict[str, Any], manifest_files: dict[str, dict[str, Any]]
) -> None:
    value = read_json(path, "license metadata", canonical=True)
    if not isinstance(value, dict) or set(value) != LICENSE_FIELDS:
        raise VerificationError("license metadata field set is invalid")
    for key in ("schemaVersion", "component", "version", "architecture", "releaseTag", "releaseSequence"):
        if value.get(key) != identity[key] or type(value.get(key)) is not type(identity[key]):
            raise VerificationError(f"license metadata {key} does not match the manifest")
    if value.get("publicationStatus") != "READY":
        raise VerificationError("license inventory is not publication-ready")
    if value.get("declaredLicense") != LICENSE_EXPRESSION or value.get("concludedLicense") != LICENSE_EXPRESSION:
        raise VerificationError("release license expression is invalid")
    expected_license_files = [LICENSE_ARCHIVE_PREFIX + name for name in RELEASE_LICENSE_FILES]
    if value.get("licenseFiles") != expected_license_files:
        raise VerificationError("license file inventory is invalid")
    entries = value.get("files")
    if not isinstance(entries, list) or len(entries) != len(manifest_files):
        raise VerificationError("per-file license inventory has the wrong size")
    paths = [entry.get("path") for entry in entries if isinstance(entry, dict)]
    if paths != sorted(manifest_files) or any(
        set(entry) != {"path", "licenseConcluded"}
        or not isinstance(entry.get("licenseConcluded"), str)
        or not entry["licenseConcluded"]
        for entry in entries
        if isinstance(entry, dict)
    ) or len(paths) != len(entries):
        raise VerificationError("per-file license inventory does not match the manifest")
    if "NOASSERTION" in json.dumps(value, sort_keys=True):
        raise VerificationError("license metadata contains unresolved assertions")


def verify_spdx_sidecar(
    path: Path,
    identity: dict[str, Any],
    manifest_files: dict[str, dict[str, Any]],
    archive_sha256: str,
    source_date_epoch: int,
) -> None:
    value = read_json(path, "SPDX metadata", canonical=True)
    if not isinstance(value, dict) or value.get("spdxVersion") != "SPDX-2.3":
        raise VerificationError("SBOM is not SPDX 2.3 JSON")
    if value.get("name") != "%s-%s-%s" % (
        COMPONENT, identity["version"], identity["architecture"]
    ):
        raise VerificationError("SPDX release identity is invalid")
    expected_created = dt.datetime.fromtimestamp(
        source_date_epoch, tz=dt.timezone.utc
    ).isoformat().replace("+00:00", "Z")
    if value.get("creationInfo", {}).get("created") != expected_created:
        raise VerificationError("SPDX timestamp does not match the source commit")
    packages = value.get("packages")
    if not isinstance(packages, list) or len(packages) != 1:
        raise VerificationError("SPDX must describe exactly one package")
    package = packages[0]
    if (
        not isinstance(package, dict)
        or package.get("name") != COMPONENT
        or package.get("versionInfo") != identity["version"]
        or package.get("filesAnalyzed") is not True
        or {"algorithm": "SHA256", "checksumValue": archive_sha256}
        not in package.get("checksums", [])
    ):
        raise VerificationError("SPDX package does not match the runtime archive")
    entries = value.get("files")
    if not isinstance(entries, list):
        raise VerificationError("SPDX file inventory is absent")
    by_path = {
        entry.get("fileName", "")[2:]: entry
        for entry in entries
        if isinstance(entry, dict) and entry.get("fileName", "").startswith("./")
    }
    if set(by_path) != set(manifest_files) or len(entries) != len(by_path):
        raise VerificationError("SPDX file inventory does not match the manifest")
    for name, manifest_entry in manifest_files.items():
        if {"algorithm": "SHA256", "checksumValue": manifest_entry["sha256"]} not in by_path[name].get("checksums", []):
            raise VerificationError(f"SPDX checksum mismatch for {name}")
    if "NOASSERTION" in json.dumps(value, sort_keys=True):
        raise VerificationError("SPDX metadata contains unresolved assertions")


def deterministic_gzip(data: bytes, source_date_epoch: int) -> bytes:
    output = io.BytesIO()
    with gzip.GzipFile(
        filename="", mode="wb", fileobj=output, compresslevel=9, mtime=source_date_epoch
    ) as stream:
        stream.write(data)
    return output.getvalue()


def verify_architecture(
    asset_dir: Path,
    architecture: str,
    expected: dict[str, Any],
    expected_source_date_epoch: int,
) -> tuple[dict[str, Any], dict[str, bytes]]:
    prefix = "%s-%s" % (COMPONENT, architecture)
    manifest_path = asset_dir / (prefix + ".runtime-manifest.json")
    regular_file_bytes(
        manifest_path, "runtime manifest", maximum=MAXIMUM_MANIFEST_BYTES
    )
    manifest = read_json(manifest_path, "runtime manifest", canonical=True)
    if not isinstance(manifest, dict) or set(manifest) != MANIFEST_FIELDS:
        raise VerificationError("runtime manifest field set is invalid")
    identity = {
        "schemaVersion": SCHEMA_VERSION,
        "component": COMPONENT,
        "version": expected["version"],
        "sourceRepository": expected["sourceRepository"],
        "sourceCommit": expected["sourceCommit"],
        "releaseTag": expected["releaseTag"],
        "releaseSequence": expected["releaseSequence"],
        "architecture": architecture,
        "guestRuntimeABI": GUEST_RUNTIME_ABI,
    }
    for key, value in identity.items():
        if manifest.get(key) != value or type(manifest.get(key)) is not type(value):
            raise VerificationError(f"runtime manifest {key} is invalid")
    uint64(manifest.get("releaseSequence"), "releaseSequence")
    archive = manifest.get("archive")
    if not isinstance(archive, dict) or set(archive) != ARCHIVE_FIELDS:
        raise VerificationError("archive reference schema is invalid")
    archive_name = prefix + ".tar"
    if archive.get("name") != archive_name:
        raise VerificationError("archive name does not match its architecture")
    archive_size = exact_int(archive.get("size"), "archive.size", minimum=1)
    if archive_size > MAXIMUM_ARCHIVE_BYTES:
        raise VerificationError("archive exceeds the host size limit")
    archive_sha256 = nonempty_string(archive.get("sha256"), "archive.sha256")
    if not SHA256_RE.fullmatch(archive_sha256):
        raise VerificationError("archive.sha256 is not lowercase SHA-256")
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries or len(entries) > MAXIMUM_FILES:
        raise VerificationError("manifest file inventory size is invalid")
    manifest_files: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != FILE_FIELDS:
            raise VerificationError(f"files[{index}] schema is invalid")
        name = safe_path(entry.get("path"), f"files[{index}].path")
        if name in manifest_files:
            raise VerificationError(f"duplicate manifest path: {name}")
        size = exact_int(entry.get("size"), f"files[{index}].size")
        if size > MAXIMUM_ARCHIVE_BYTES:
            raise VerificationError(f"files[{index}] exceeds the host size limit")
        digest = nonempty_string(entry.get("sha256"), f"files[{index}].sha256")
        if not SHA256_RE.fullmatch(digest):
            raise VerificationError(f"files[{index}].sha256 is invalid")
        mode = exact_int(entry.get("mode"), f"files[{index}].mode")
        if mode not in (0o644, 0o755):
            raise VerificationError(f"files[{index}].mode is unsafe")
        manifest_files[name] = {"path": name, "size": size, "sha256": digest, "mode": mode}
    if [entry["path"] for entry in entries] != sorted(manifest_files):
        raise VerificationError("manifest file inventory is not lexically sorted")
    expected_modes = required_files(architecture)
    if {name: entry["mode"] for name, entry in manifest_files.items()} != expected_modes:
        raise VerificationError("manifest file set or modes do not match platform policy")
    archive_path = asset_dir / archive_name
    archive_data = regular_file_bytes(
        archive_path, "runtime archive", maximum=MAXIMUM_ARCHIVE_BYTES
    )
    if len(archive_data) != archive_size or sha256_bytes(archive_data) != archive_sha256:
        raise VerificationError("runtime archive size or SHA-256 does not match the manifest")
    source_date_epoch = verify_source_sidecar(
        asset_dir / (prefix + ".source.json"), identity, expected_source_date_epoch
    )
    contents = verify_archive(
        archive_path, manifest_files, source_date_epoch, architecture
    )
    gzip_data = regular_file_bytes(asset_dir / (prefix + ".tar.gz"), "gzip archive")
    if gzip_data != deterministic_gzip(archive_data, source_date_epoch):
        raise VerificationError("gzip sidecar is not the deterministic archive copy")
    verify_license_sidecar(
        asset_dir / (prefix + ".licenses.json"), identity, manifest_files
    )
    verify_spdx_sidecar(
        asset_dir / (prefix + ".spdx.json"),
        identity,
        manifest_files,
        archive_sha256,
        source_date_epoch,
    )
    return manifest, contents


def expected_asset_names() -> set[str]:
    result = {"GuestEnvironmentCatalog.signed.json"}
    for architecture in ARCHITECTURES:
        prefix = "%s-%s" % (COMPONENT, architecture)
        result.update(
            {
                prefix + ".tar",
                prefix + ".tar.gz",
                prefix + ".runtime-manifest.json",
                prefix + ".source.json",
                prefix + ".licenses.json",
                prefix + ".spdx.json",
            }
        )
    return result


def verify_release(args: argparse.Namespace) -> None:
    asset_dir = args.asset_dir.resolve()
    if not asset_dir.is_dir():
        raise VerificationError("release asset directory is missing")
    sequence = uint64(args.expected_sequence, "expected release sequence")
    if not VERSION_RE.fullmatch(args.expected_version):
        raise VerificationError("expected version is invalid")
    if not REPOSITORY_RE.fullmatch(args.expected_repository):
        raise VerificationError("expected repository is invalid")
    if not COMMIT_RE.fullmatch(args.expected_commit):
        raise VerificationError("expected commit must be a 40-character Git SHA")
    expected_tag = "%s-v%s-%d" % (COMPONENT, args.expected_version, sequence)
    if args.expected_tag != expected_tag:
        raise VerificationError("expected release tag does not match version and sequence")
    children = list(asset_dir.iterdir())
    if any(not (path.is_file() or path.is_symlink()) for path in children):
        raise VerificationError("unsigned release contains a non-file top-level asset")
    observed_names = {path.name for path in children}
    if observed_names != expected_asset_names():
        missing = sorted(expected_asset_names() - observed_names)
        extra = sorted(observed_names - expected_asset_names())
        raise VerificationError(
            f"unsigned release asset set mismatch; missing={missing}, extra={extra}"
        )
    expected = {
        "version": args.expected_version,
        "sourceRepository": args.expected_repository,
        "sourceCommit": args.expected_commit,
        "releaseTag": args.expected_tag,
        "releaseSequence": sequence,
    }
    manifests: list[dict[str, Any]] = []
    contents_by_architecture: dict[str, dict[str, bytes]] = {}
    for architecture in ARCHITECTURES:
        manifest, contents = verify_architecture(
            asset_dir,
            architecture,
            expected,
            args.expected_source_date_epoch,
        )
        manifests.append(manifest)
        contents_by_architecture[architecture] = contents
    common_paths = set(contents_by_architecture["aarch64"]) & set(
        contents_by_architecture["x86_64"]
    )
    for name in common_paths:
        if name.startswith("guest/bootstrap/dist/") or name.startswith("guest/guestd/dist/"):
            continue
        if contents_by_architecture["aarch64"][name] != contents_by_architecture["x86_64"][name]:
            raise VerificationError(f"architecture-independent payload differs: {name}")
    environment_envelope = regular_file_bytes(
        asset_dir / "GuestEnvironmentCatalog.signed.json",
        "environment catalog envelope",
    )
    for architecture, contents in contents_by_architecture.items():
        verify_catalog_envelope(
            contents["Resources/GuestEnvironmentCatalog.json"],
            contents["Resources/GuestEnvironmentCatalogPublicKey.txt"],
            environment_envelope,
            f"{architecture} environment catalog",
            args.openssl,
        )
        verify_catalog_envelope(
            contents["InstallAdapters/catalog.json"],
            contents["InstallAdapters/catalog-public-key.txt"],
            contents["InstallAdapters/catalog.signed.json"],
            f"{architecture} installation catalog",
            args.openssl,
        )
    if manifests[0]["releaseSequence"] != manifests[1]["releaseSequence"]:
        raise VerificationError("architectures do not share one release sequence")


def parse_uint64_argument(value: str) -> int:
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise argparse.ArgumentTypeError("must be a positive decimal integer without leading zeroes")
    parsed = int(value, 10)
    if parsed > MAXIMUM_RELEASE_SEQUENCE:
        raise argparse.ArgumentTypeError("must not exceed UInt64.max")
    return parsed


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asset-dir", required=True, type=Path)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-tag", required=True)
    parser.add_argument("--expected-sequence", required=True, type=parse_uint64_argument)
    parser.add_argument("--expected-repository", required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-source-date-epoch", required=True, type=int)
    parser.add_argument("--openssl", default="openssl")
    return parser.parse_args()


def main() -> None:
    try:
        verify_release(parse_arguments())
    except VerificationError as error:
        raise SystemExit("platform release verification failed: %s" % error) from error
    print("guest-platform unsigned release verification: ok")


if __name__ == "__main__":
    main()
