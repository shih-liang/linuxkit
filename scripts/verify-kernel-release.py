#!/usr/bin/env python3
"""Verify an existing kernel release without modifying it or its tag."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


class VerificationError(Exception):
    pass


def gh(*arguments):
    try:
        return subprocess.run(
            ["gh", *arguments], capture_output=True, text=True, timeout=600
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(f"GitHub request failed: {error}") from error


def api(endpoint, allow_missing=False):
    result = gh("api", endpoint, "--include")
    headers, separator, body = result.stdout.replace("\r\n", "\n").partition("\n\n")
    status = re.match(r"HTTP/\S+ (\d{3})(?:\s|$)", headers)
    if not separator or not status:
        raise VerificationError(f"GitHub API returned no HTTP response for {endpoint}")
    code = int(status[1])
    if code == 404 and allow_missing:
        return None
    if code != 200 or result.returncode:
        raise VerificationError(f"GitHub API returned HTTP {code} for {endpoint}")
    try:
        value = json.loads(body)
    except ValueError as error:
        raise VerificationError(f"Invalid GitHub JSON for {endpoint}") from error
    if not isinstance(value, (dict, list)):
        raise VerificationError(f"Invalid GitHub response for {endpoint}")
    return value


def payload_names(version):
    return {
        f"lighthouse-linux-{version}-arm64-16k.Image",
        f"lighthouse-linux-{version}-arm64-4k.Image",
        f"lighthouse-initramfs-{version}-arm64.img",
        f"lighthouse-linux-{version}-arm64.tar.gz",
        "config-aarch64", "config-aarch64-4k",
        "System.map", "System.map-4k", "Module.symvers", "Module.symvers-4k",
        "linuxkit-kernel-licenses.tar.gz", "linuxkit-recovery-licenses.tar.gz",
        "installation-catalog.json",
    }


def verify(tag, expected_commit=None, require_existing=False):
    match = re.fullmatch(r"lighthouse-kernel-v(\d+\.\d+\.\d+)-[1-9]\d*", tag)
    if not match:
        raise VerificationError("Invalid kernel release tag")
    if expected_commit and not re.fullmatch(r"[0-9a-f]{40}", expected_commit):
        raise VerificationError("Expected commit must be a full Git commit SHA")
    repository = os.environ.get("GH_REPO", "")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise VerificationError("GH_REPO must identify owner/repository")
    prefix = f"repos/{repository}"
    commit = api(f"{prefix}/commits/{tag}", allow_missing=True)
    commit_sha = commit.get("sha") if isinstance(commit, dict) else None
    if commit is not None and (not isinstance(commit_sha, str) or not re.fullmatch(r"[0-9a-f]{40}", commit_sha)):
        raise VerificationError("Tag did not resolve to a valid commit")
    if commit_sha and expected_commit and commit_sha != expected_commit:
        raise VerificationError(f"Tag points to {commit_sha}, not {expected_commit}")

    release = api(f"{prefix}/releases/tags/{tag}", allow_missing=True)
    if release is None:
        # The tag endpoint may hide drafts; authenticated listing must confirm absence.
        page = 1
        while True:
            releases = api(f"{prefix}/releases?per_page=100&page={page}")
            if not isinstance(releases, list) or any(not isinstance(item, dict) for item in releases):
                raise VerificationError("Invalid GitHub release listing")
            matches = [item for item in releases if item.get("tag_name") == tag]
            if len(matches) > 1:
                raise VerificationError("Duplicate releases for the requested tag")
            if matches:
                release = matches[0]
                break
            if len(releases) < 100:
                break
            page += 1
    if release is None:
        if require_existing:
            raise VerificationError(f"Release {tag} does not exist")
        return False
    if not isinstance(release, dict) or release.get("tag_name") != tag:
        raise VerificationError("Release tag does not match the request")
    if release.get("draft") is not False:
        raise VerificationError("Existing release is a draft or has no published state")
    if not commit_sha:
        raise VerificationError("Published release has no resolvable tag commit")

    payloads = payload_names(match[1])
    required = payloads | {"SHA256SUMS"}
    assets = release.get("assets", [])
    if not isinstance(assets, list) or any(not isinstance(asset, dict) for asset in assets):
        raise VerificationError("Invalid release asset list")
    names = [asset.get("name") for asset in assets]
    if (len(names) != len(required)
            or any(not isinstance(name, str) or name not in required for name in names)
            or set(names) != required):
        raise VerificationError("Release must contain all 14 standard assets exactly once")
    for asset in assets:
        if type(asset.get("size")) is not int or asset["size"] <= 0 or asset.get("state") != "uploaded":
            raise VerificationError(f"Asset is empty or not uploaded: {asset['name']}")
    metadata = {asset["name"]: asset for asset in assets}
    if metadata["SHA256SUMS"]["size"] > 65536:
        raise VerificationError("SHA256SUMS is unexpectedly large")

    with tempfile.TemporaryDirectory(prefix="linportal-kernel-release-") as directory:
        arguments = ["release", "download", tag, "--repo", repository, "--dir", directory]
        for name in sorted(required):
            arguments.extend(["--pattern", name])
        result = gh(*arguments)
        if result.returncode:
            raise VerificationError(f"Release download failed: {result.stderr.strip()}")
        hashes = {}
        for name in sorted(required):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size != metadata[name]["size"]:
                raise VerificationError(f"Missing asset or size mismatch: {name}")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            hashes[name] = digest.hexdigest()
            upstream_digest = metadata[name].get("digest")
            if upstream_digest is not None and upstream_digest != f"sha256:{hashes[name]}":
                raise VerificationError(f"GitHub digest mismatch: {name}")
        try:
            manifest = (Path(directory) / "SHA256SUMS").read_text(encoding="ascii")
        except UnicodeError as error:
            raise VerificationError("SHA256SUMS is not ASCII") from error
        checksums = {}
        for line in manifest.splitlines():
            entry = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
            if not entry or entry[2] not in payloads or entry[2] in checksums:
                raise VerificationError("Malformed, unexpected, or duplicate SHA256SUMS entry")
            checksums[entry[2]] = entry[1].lower()
        if set(checksums) != payloads:
            raise VerificationError("SHA256SUMS does not cover all 13 payloads")
        for name, checksum in checksums.items():
            if hashes[name] != checksum:
                raise VerificationError(f"SHA256SUMS mismatch: {name}")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--expected-commit")
    parser.add_argument("--require-existing", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        found = verify(arguments.tag, arguments.expected_commit, arguments.require_existing)
    except (VerificationError, OSError) as error:
        print(f"Kernel release verification failed: {error}", file=sys.stderr)
        return 1
    print("true" if found else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
