#!/usr/bin/env python3
"""Fail closed when guest-platform release inputs lack provenance/notices."""

from __future__ import annotations

import fnmatch
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
INVENTORY_PATH = ROOT / "LICENSES/source-inventory.json"
IGNORED_DIRECTORY_NAMES = {".git", "build", "dist", "__pycache__"}


def fail(message: str) -> None:
    raise SystemExit(f"license inventory error: {message}")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matched(path: str, entry: dict[str, object]) -> bool:
    covered = entry.get("coveredPaths")
    excluded = entry.get("excludedPaths", [])
    if not isinstance(covered, list) or not isinstance(excluded, list):
        return False
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in covered) and not any(
        fnmatch.fnmatchcase(path, pattern) for pattern in excluded
    )


def platform_source_files() -> list[str]:
    paths = [
        ".github/workflows/build-linux.yml",
        "scripts/sign-installation-catalog.py",
    ]
    for relative in ("LICENSES", "Resources", "adapters", "guest-platform"):
        base = ROOT / relative
        if not base.exists():
            continue
        for item in base.rglob("*"):
            if not item.is_file() or any(
                part in IGNORED_DIRECTORY_NAMES for part in item.relative_to(ROOT).parts
            ):
                continue
            paths.append(item.relative_to(ROOT).as_posix())
    return sorted(set(path for path in paths if (ROOT / path).is_file()))


def main() -> None:
    try:
        inventory = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(str(error))
    if inventory.get("schemaVersion") != 1:
        fail("schemaVersion must be 1")
    entries = inventory.get("entries")
    if not isinstance(entries, list):
        fail("entries must be an array")
    by_id = {entry.get("id"): entry for entry in entries if isinstance(entry, dict)}
    expected_ids = {
        "linuxkit-platform-original",
        "zig-0.16.0",
        "musl-zig-0.16.0",
    }
    if set(by_id) != expected_ids:
        fail(f"expected exactly {sorted(expected_ids)}, found {sorted(str(key) for key in by_id)}")

    original = by_id["linuxkit-platform-original"]
    if original.get("licenseExpression") != "LicenseRef-LightHouse-Original":
        fail("project-original files must retain LicenseRef-LightHouse-Original")
    if "does not infer" not in str(original.get("licenseNotice", "")):
        fail("project-original rights notice is missing")
    for policy_path in (
        ".github/workflows/build-linux.yml",
        "scripts/sign-installation-catalog.py",
    ):
        matches = [entry["id"] for entry in entries if matched(policy_path, entry)]
        if matches != ["linuxkit-platform-original"]:
            fail(f"release policy provenance is missing or ambiguous: {policy_path}")

    zig = by_id["zig-0.16.0"]
    musl = by_id["musl-zig-0.16.0"]
    if zig.get("licenseExpression") != "MIT":
        fail("Zig 0.16.0 must retain its MIT license")
    if musl.get("licenseExpression") != "LicenseRef-musl-COPYRIGHT":
        fail("musl must retain its exact bundled COPYRIGHT terms")
    expected_artifacts = [
        "guest/bootstrap/dist/nativepipe-bootstrap-*",
        "guest/guestd/dist/nativepipe-guestd-*",
    ]
    for entry in (zig, musl):
        if entry.get("releaseArtifactPaths") != expected_artifacts:
            fail(f"{entry['id']} does not cover both static guest executables")
        license_file = entry.get("licenseFile")
        expected_digest = entry.get("licenseFileSHA256")
        if not isinstance(license_file, str) or not isinstance(expected_digest, str):
            fail(f"{entry['id']} has incomplete notice metadata")
        path = ROOT / license_file
        if not path.is_file() or sha256(path) != expected_digest:
            fail(f"{entry['id']} notice is missing or differs from the pinned toolchain")

    release_files = inventory.get("releaseLicenseFiles")
    if not isinstance(release_files, list) or not release_files:
        fail("releaseLicenseFiles must be a non-empty array")
    for relative in release_files:
        if not isinstance(relative, str) or not (ROOT / relative).is_file():
            fail(f"missing release notice: {relative!r}")

    third_party_notices = {
        str(zig["licenseFile"]),
        str(musl["licenseFile"]),
    }
    uncovered: list[str] = []
    ambiguous: list[str] = []
    for path in platform_source_files():
        if path in third_party_notices:
            continue
        matches = [entry["id"] for entry in entries if matched(path, entry)]
        if not matches:
            uncovered.append(path)
        elif len(matches) != 1:
            ambiguous.append(f"{path}: {matches}")
    if uncovered:
        fail("uncovered platform sources:\n  " + "\n  ".join(uncovered))
    if ambiguous:
        fail("platform sources with ambiguous provenance:\n  " + "\n  ".join(ambiguous))

    print(
        "license inventory OK: "
        f"{len(platform_source_files())} platform files, exact Zig 0.16.0 and musl notices"
    )


if __name__ == "__main__":
    main()
