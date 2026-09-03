#!/usr/bin/env python3
import json
import pathlib
import re
from urllib.parse import urlparse

root = pathlib.Path(__file__).resolve().parent
catalog = json.loads((root / "catalog.json").read_text())
assert catalog["schemaVersion"] == 2
assert catalog["revision"] >= 1
required_distros = {"ubuntu", "fedora", "archlinux-arm"}
ids = {item["id"] for item in catalog["distributions"]}
assert len(ids) == len(catalog["distributions"])
assert required_distros <= ids

all_software = set()
for distro in catalog["distributions"]:
    assert re.fullmatch(r"[a-z0-9][a-z0-9._-]*", distro["id"])
    assert distro["architecture"] == "arm64"
    assert (root / distro["adapter"]).is_file()
    assert pathlib.PurePath(distro["adapter"]).name == distro["adapter"]
    assert "version" not in distro and "rootfs" not in distro
    boot_arguments = distro["bootArguments"]
    assert sum(value.startswith("root=") for value in boot_arguments) == 1
    assert all(value and not any(char.isspace() for char in value)
               for value in boot_arguments)
    assert "repair)" in (root / distro["adapter"]).read_text()

    source = distro["source"]
    assert not ({"value", "artifactURL", "downloadURL", "releaseURL"} & source.keys())
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
ubuntu_adapter = (root / ubuntu["adapter"]).read_text()
assert "wine64:amd64" in ubuntu_adapter
assert "libgl1-mesa-dri" in ubuntu_adapter
assert "fex-emu-binfmt" not in ubuntu_adapter
fedora_adapter = (root / "fedora.sh").read_text()
arch_adapter = (root / "archlinux.sh").read_text()
assert "mesa-dri-drivers" in fedora_adapter
assert "xcb-util-cursor" in fedora_adapter
assert "mesa vulkan-virtio" in arch_adapter
assert "xcb-util-cursor" in arch_adapter
print(f"validated {len(catalog['distributions'])} dynamic installation sources")
