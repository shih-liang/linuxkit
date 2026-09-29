#!/usr/bin/env python3
import json
import pathlib
import re
import subprocess
from urllib.parse import urlparse

root = pathlib.Path(__file__).resolve().parent
catalog = json.loads((root / "catalog.json").read_text())
assert catalog["schemaVersion"] == 4
assert catalog["revision"] >= 1
required_distros = {"alpine", "debian", "ubuntu", "fedora", "archlinux-arm"}
ids = {item["id"] for item in catalog["distributions"]}
assert len(ids) == len(catalog["distributions"])
assert required_distros <= ids

all_software = set()
for distro in catalog["distributions"]:
    assert re.fullmatch(r"[a-z0-9][a-z0-9._-]*", distro["id"])
    assert distro["architecture"] == "arm64"
    assert distro["adapter"] == "nativepipe-install"
    assert distro["format"] in {"tar", "oci"}
    assert pathlib.PurePath(distro["adapter"]).name == distro["adapter"]
    assert "version" not in distro and "rootfs" not in distro
    boot_arguments = distro["bootArguments"]
    assert sum(value.startswith("root=") for value in boot_arguments) == 1
    assert all(value and not any(char.isspace() for char in value)
               for value in boot_arguments)

    source = distro["source"]
    assert not ({"value", "artifactURL", "downloadURL", "releaseURL"} & source.keys())
    assert re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", source["versionSeries"])
    index = urlparse(source["indexURL"])
    assert index.scheme == "https" and index.hostname
    assert re.compile(source["artifactPattern"]).groups >= 1
    assert source["checksumAlgorithm"] in {"sha256", "md5"}
    assert 1 <= source.get("maximumItems", 32) <= 64
    if source["kind"] == "directory":
        assert ("checksumFile" in source) != ("checksumSuffix" in source)
        assert "checksumKey" not in source and "filters" not in source
        if "releasePattern" in source:
            assert re.compile(source["releasePattern"]).groups >= 1
    elif source["kind"] == "jsonArray":
        assert source["urlKey"] and source["checksumKey"] and source["filters"]
        assert "checksumFile" not in source and "checksumSuffix" not in source
    elif source["kind"] == "debianOCI":
        assert index.hostname == "api.github.com"
        assert index.path.startswith("/repos/debuerreotype/docker-debian-artifacts/commits/")
        assert source["checksumAlgorithm"] == "sha256"
        assert re.fullmatch(r"[a-z][a-z0-9-]*/", source["releaseSubpath"])
        assert source["releaseSubpath"] not in {"stable/", "oldstable/", "testing/", "unstable/", "sid/"}
    else:
        raise AssertionError(f"unsupported release source {source['kind']}")

    software = {item["id"] for item in distro["software"]}
    assert len(software) == len(distro["software"])
    assert all(re.fullmatch(r"[a-z0-9][a-z0-9._-]*", item) for item in software)
    all_software |= software

assert {"steam", "x86_64", "wine"} <= all_software
ubuntu = next(item for item in catalog["distributions"] if item["id"] == "ubuntu")
ubuntu_software = {item["id"]: item for item in ubuntu["software"]}
assert ubuntu_software["wine"].get("requiresRosetta") is True
assert ubuntu_software["amd64-rootfs"].get("requiresRosetta") is True
amd64 = ubuntu["amd64Source"]
assert amd64["checksumAlgorithm"] == "sha256"
assert amd64["indexURL"] == ubuntu["source"]["indexURL"]
assert amd64["versionSeries"] == ubuntu["source"]["versionSeries"]
assert amd64["artifactPattern"] == ubuntu["source"]["artifactPattern"].replace("arm64", "amd64")
print(f"validated {len(catalog['distributions'])} C installer sources with approved release series")
